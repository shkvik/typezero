#include "compiler/semantic/generics/alias_instantiator/alias_instantiator.h"
#include "compiler/semantic/bodies/classes/class_bases/class_bases.h"
#include "compiler/semantic/generics/generic_check_helpers/generic_check_helpers.h"
#include "compiler/semantic/generics/type_params/type_params.h"
#include "compiler/semantic/semantic_parts/semantic_parts.h"
#include "compiler/semantic/types/type_resolver/type_resolver.h"
#include "llvm/ADT/STLExtras.h"

namespace tz {

namespace {

constexpr unsigned kMaxAliasDepth = 50;

constexpr unsigned kMaxTailCalls = 1000;

}

AliasInstantiator::AliasInstantiator(SemanticState &owner, SemanticParts &all)
    : state(owner) {
  this->classBases = &all.classBases;
  this->typeParams = &all.typeParams;
  this->typeResolver = &all.typeResolver;
}

AliasInstantiator::ResultArgumentCount
AliasInstantiator::argumentCount(const TypeAliasDecl *alias, size_t count, size_t got) const {
  if (got > count) {
    auto message = ErrorTextAliasCount::tooMany(alias->name, count, got);

    return err(AliasInstantiatorArgumentCountError{
        .code = AliasInstantiatorArgumentCountErrorCode::TOO_MANY_TYPE_ARGUMENTS,
        .message = message,
    });
  }

  return ok(got);
}

AliasInstantiator::ResultDefaultExists
AliasInstantiator::defaultExists(const TypeAliasDecl *alias, size_t count,
                                 const Type *fallback) const {
  if (fallback == nullptr) {
    auto message = ErrorTextAliasDefault::needs(alias->name, count);

    return err(AliasInstantiatorDefaultExistsError{
        .code = AliasInstantiatorDefaultExistsErrorCode::TYPE_ARGUMENTS_MISSING,
        .message = message,
    });
  }

  return ok(fallback);
}

AliasInstantiator::ResultArgumentMatch
AliasInstantiator::argumentMatch(const TypeAliasDecl *alias, llvm::ArrayRef<const Type *> params,
                                 llvm::ArrayRef<const Type *> args, size_t index) {
  const Type *arg = args[index];
  const Type *param = params[index];
  if (!this->typeParams->isSatisfying(arg, param, params, args)) {
    auto constraint = GenericCheckHelpers::constraintSpelling(param, &alias->typeParams[index]);
    auto message =
        ErrorTextAliasMatch::unsatisfied(arg->name(), constraint, param->name(), alias->name);

    return err(AliasInstantiatorArgumentMatchError{
        .code = AliasInstantiatorArgumentMatchErrorCode::CONSTRAINT_UNSATISFIED,
        .message = message,
    });
  }

  return ok(arg);
}

AliasInstantiator::ResultAliasDistinct
AliasInstantiator::notResolving(const TypeAliasDecl *alias, const AliasInstance &entry) const {
  if (entry.resolving && entry.self == nullptr) {
    auto message = ErrorTextAliasSelf::itself(alias->name);

    return err(AliasInstantiatorNotResolvingError{
        .code = AliasInstantiatorNotResolvingErrorCode::REFERS_TO_ITSELF,
        .message = message,
    });
  }

  return ok(alias);
}

AliasInstantiator::ResultDepthInBounds AliasInstantiator::depthInBounds(const TypeAliasDecl *alias,
                                                                        unsigned calls,
                                                                        bool isConditional) const {
  bool isDeep =
      this->state.getAliasStore().getAliasDepth() >= kMaxAliasDepth || calls > kMaxTailCalls;
  if (isDeep && !isConditional) {
    auto message = ErrorTextAliasDepth::endless(alias->name);

    return err(AliasInstantiatorDepthInBoundsError{
        .code = AliasInstantiatorDepthInBoundsErrorCode::ENDLESS_ALIAS,
        .message = message,
    });
  }
  if (isDeep && calls > kMaxTailCalls) {
    auto message = ErrorTextAliasDepth::tailCalls(alias->name, kMaxTailCalls);

    return err(AliasInstantiatorDepthInBoundsError{
        .code = AliasInstantiatorDepthInBoundsErrorCode::TOO_MANY_TAIL_CALLS,
        .message = message,
    });
  }
  if (isDeep) {
    auto message = ErrorTextAliasDepth::nests(alias->name, kMaxAliasDepth, kMaxTailCalls);

    return err(AliasInstantiatorDepthInBoundsError{
        .code = AliasInstantiatorDepthInBoundsErrorCode::TOO_DEEP,
        .message = message,
    });
  }

  return ok(calls);
}

std::vector<const Type *> AliasInstantiator::aliasTypeParams(TypeAliasDecl *alias) {
  auto &aliases = this->state.getAliasStore();
  auto known = aliases.findAliasParams(alias);
  if (known.has_value()) {
    return known->params;
  }
  this->state.getScopeStack().openTypeScope();
  auto defaults = std::vector<const Type *>();
  auto params = this->typeParams->declareTypeParams(alias->typeParams, &defaults);
  this->state.getScopeStack().closeTypeScope();
  auto declared = AliasParams{.params = std::move(params), .defaults = std::move(defaults)};
  aliases.rememberAliasParams(alias, std::move(declared));

  return aliases.findAliasParams(alias)->params;
}

const Type *AliasInstantiator::instantiateAlias(TypeAliasDecl *alias,
                                                std::vector<const Type *> args, llvm::SMLoc loc) {
  auto &aliases = this->state.getAliasStore();
  if (aliases.getAliasDepth() == 0) {
    aliases.rememberAliasUse(loc);
  }
  auto run =
      AliasRun{.alias = alias, .args = std::move(args), .loc = loc, .defaults = {}, .chain = {}};
  const Type *type = nullptr;
  for (unsigned calls = 0; type == nullptr; ++calls) {
    type = this->aliasStep(run, calls);
  }
  for (AliasInstance *entry : run.chain) {
    entry->resolving = false;
    entry->type = type;
  }

  return type;
}

const Type *AliasInstantiator::aliasStep(AliasRun &run, unsigned calls) {
  const auto *failed = this->failedAliasArgs(run);
  if (failed != nullptr) {
    return failed;
  }
  AliasInstance &entry = *this->state.getAliasStore().recordAliasInstance(run.alias, run.args);
  const auto *reused = this->reusedInstance(entry);
  if (reused != nullptr) {
    return reused;
  }
  if (!this->isAliasDepthFitting(run.alias, entry, calls)) {
    return Type::error();
  }
  auto display = this->aliasDisplay(run);
  entry.resolving = true;
  run.chain.push_back(&entry);
  const auto *type = this->resolveAliasBody(run, entry, display);
  if (type == nullptr && this->state.getTypeContext().hasTailCall()) {
    auto tail = this->state.getTypeContext().takeTailCall();
    run.alias = tail.alias;
    run.args = std::move(tail.args);
    run.loc = run.alias->loc();
    return nullptr;
  }
  if (type == nullptr) {
    type = Type::error();
  }
  this->rememberAliasType(run, type, display);

  return type;
}

const Type *AliasInstantiator::failedAliasArgs(AliasRun &run) {
  this->aliasTypeParams(run.alias);
  AliasParams declared = *this->state.getAliasStore().findAliasParams(run.alias);
  std::vector<const Type *> params = declared.params;
  run.defaults = declared.defaults;
  auto counted = this->argumentCount(run.alias, params.size(), run.args.size());
  if (counted.isErr()) {
    this->state.getDiagnostics().error(run.loc, counted.error().message);

    return Type::error();
  }
  for (size_t index = run.args.size(); index < params.size(); ++index) {
    auto exists = this->defaultExists(run.alias, params.size(), run.defaults[index]);
    if (exists.isErr()) {
      this->state.getDiagnostics().error(run.loc, exists.error().message);

      return Type::error();
    }
    const auto *filled = this->typeParams->substitute(run.defaults[index], params, run.args);
    run.args.push_back(filled);
  }
  for (size_t index = 0; index < params.size(); ++index) {
    if (run.args[index]->isError()) {
      return run.args[index];
    }
    auto matched = this->argumentMatch(run.alias, params, run.args, index);
    if (matched.isErr()) {
      this->state.getDiagnostics().error(run.loc, matched.error().message);

      return Type::error();
    }
  }

  return nullptr;
}

const Type *AliasInstantiator::reusedInstance(AliasInstance &entry) const {
  if (entry.type != nullptr) {
    return entry.type;
  }
  if (entry.resolving && entry.self != nullptr) {
    entry.recursive = true;

    return entry.self;
  }

  return nullptr;
}

bool AliasInstantiator::isAliasDepthFitting(const TypeAliasDecl *alias, const AliasInstance &entry,
                                            unsigned calls) {
  auto distinct = this->notResolving(alias, entry);
  if (distinct.isErr()) {
    this->state.getDiagnostics().error(alias->loc(), distinct.error().message);

    return false;
  }
  bool isConditional = alias->type.kind() == TypeRefKind::CONDITIONAL_REF;
  auto bounded = this->depthInBounds(alias, calls, isConditional);
  if (bounded.isOk()) {
    return true;
  }
  auto neededHere = ErrorTextAliasDepth::neededHere();
  this->state.getDiagnostics().error(alias->loc(), bounded.error().message);
  auto use = this->state.getAliasStore().getAliasUse();
  if (isConditional && use.isValid() && use != alias->loc()) {
    this->state.getDiagnostics().note(use, neededHere);
  }

  return false;
}

std::string AliasInstantiator::aliasDisplay(const AliasRun &run) const {
  size_t shown = GenericCheckHelpers::shownArgumentCount(run.defaults, run.args);
  if (shown == 0) {
    return run.alias->name.str();
  }
  auto shownArgs = llvm::ArrayRef<const Type *>(run.args).take_front(shown);
  auto list = GenericCheckHelpers::typeNames(shownArgs, ", ");

  return run.alias->name.str() + "<" + list + ">";
}

const Type *AliasInstantiator::resolveAliasBody(const AliasRun &run, AliasInstance &entry,
                                                const std::string &display) {
  TypeAliasDecl *alias = run.alias;
  this->state.getAliasStore().enterAlias();
  auto *home = this->state.getModuleStates().findDeclModule(alias);
  bool isMoved = home != nullptr && home != this->state.getModuleStates().getCurrentModule();
  if (isMoved) {
    const auto &homeTypes = this->state.getModuleStates().getModuleState(home).types;
    this->state.getScopeStack().enterOnlyTypes(homeTypes);
  }
  this->state.getScopeStack().openTypeScope();
  this->typeParams->bindTypeParams(alias->typeParams, run.args);
  const Type *type = alias->type.kind() == TypeRefKind::OBJECT_REF
                         ? this->objectAliasType(alias, entry, display)
                         : this->aliasTypeInner(alias);
  this->state.getScopeStack().closeTypeScope();
  if (isMoved) {
    this->state.getScopeStack().leaveOnlyTypes();
  }
  this->state.getAliasStore().leaveAlias();

  return type;
}

const Type *AliasInstantiator::objectAliasType(TypeAliasDecl *alias, AliasInstance &entry,
                                               const std::string &display) {
  entry.self = Type::declareObject(display);
  auto bases = std::vector<const Type *>();
  auto overloads = std::vector<Overload>();
  std::vector<Field> fields = alias->bases.empty()
                                  ? this->typeResolver->resolveFields(alias->type, &overloads)
                                  : this->classBases->interfaceFields(alias, bases, overloads);
  const Type *type = this->definedObject(entry, std::move(fields), std::move(overloads), display);
  for (const Type *base : bases) {
    if (!type->isError()) {
      this->classBases->addBase(type, base);
    }
  }

  return type;
}

const Type *AliasInstantiator::definedObject(const AliasInstance &entry, std::vector<Field> fields,
                                             std::vector<Overload> overloads,
                                             const std::string &display) const {
  if (entry.recursive) {
    entry.self->defineFields(std::move(fields), std::move(overloads));

    return entry.self;
  }

  return Type::object(std::move(fields), display, std::move(overloads));
}

const Type *AliasInstantiator::aliasTypeInner(const TypeAliasDecl *alias) {
  bool isConditional = alias->type.kind() == TypeRefKind::CONDITIONAL_REF;
  if (isConditional) {
    this->state.getTypeContext().allowTailCall();
  } else {
    this->state.getTypeContext().forbidTailCall();
  }
  const auto *type = this->typeResolver->resolveTypeInner(alias->type);
  this->state.getTypeContext().forbidTailCall();

  return type;
}

void AliasInstantiator::rememberAliasType(const AliasRun &run, const Type *type,
                                          const std::string &display) {
  auto &aliases = this->state.getAliasStore();
  if (type->isObject() && !type->isError() && aliases.findAliasOrigin(type) == nullptr) {
    aliases.rememberAliasOrigin(type, run.alias, run.args);
  }
  if (type->isUnion() && aliases.findBuiltinAlias(run.alias->name) == run.alias) {
    Type::nameUnion(type, display);
  }
}

}
