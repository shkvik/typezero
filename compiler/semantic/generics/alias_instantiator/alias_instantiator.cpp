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
  this->classBases = &all.getClassBases();
  this->typeParams = &all.getTypeParams();
  this->typeResolver = &all.getTypeResolver();
}

std::string AliasInstantiator::typeList(llvm::ArrayRef<const Type *> types) {
  std::string text;
  for (const Type *t : types) {
    text += (text.empty() ? "" : ", ") + t->name().str();
  }

  return text;
}

AliasInstantiator::ResultArgumentCount AliasInstantiator::argumentCount(const TypeAliasDecl *alias,
                                                                        size_t count, size_t got) {
  if (got > count) {
    std::string message = ErrorTextAliasCount::tooMany(alias->name, count, got);

    return err(AliasInstantiatorArgumentCountError{
        .code = AliasInstantiatorArgumentCountErrorCode::TOO_MANY_TYPE_ARGUMENTS,
        .message = message,
    });
  }

  return ok(got);
}

AliasInstantiator::ResultDefaultExists
AliasInstantiator::defaultExists(const TypeAliasDecl *alias, size_t count, const Type *fallback) {
  if (!fallback) {
    std::string message = ErrorTextAliasDefault::needs(alias->name, count);

    return err(AliasInstantiatorDefaultExistsError{
        .code = AliasInstantiatorDefaultExistsErrorCode::TYPE_ARGUMENTS_MISSING,
        .message = message,
    });
  }

  return ok(fallback);
}

AliasInstantiator::ResultArgumentMatch
AliasInstantiator::argumentMatch(const TypeAliasDecl *alias, llvm::ArrayRef<const Type *> params,
                                 llvm::ArrayRef<const Type *> args, size_t k) {
  if (!this->typeParams->satisfies(args[k], params[k], params, args)) {
    std::string constraint =
        GenericCheckHelpers::constraintSpelling(params[k], &alias->typeParams[k]);
    std::string message = ErrorTextAliasMatch::unsatisfied(args[k]->name(), constraint,
                                                           params[k]->name(), alias->name);

    return err(AliasInstantiatorArgumentMatchError{
        .code = AliasInstantiatorArgumentMatchErrorCode::CONSTRAINT_UNSATISFIED,
        .message = message,
    });
  }

  return ok(args[k]);
}

AliasInstantiator::ResultAliasDistinct AliasInstantiator::notResolving(const TypeAliasDecl *alias,
                                                                       const AliasInstance &entry) {
  if (entry.resolving && !entry.self) {
    std::string message = ErrorTextAliasSelf::itself(alias->name);

    return err(AliasInstantiatorNotResolvingError{
        .code = AliasInstantiatorNotResolvingErrorCode::REFERS_TO_ITSELF,
        .message = message,
    });
  }

  return ok(alias);
}

AliasInstantiator::ResultDepthInBounds AliasInstantiator::depthInBounds(const TypeAliasDecl *alias,
                                                                        unsigned calls,
                                                                        bool conditional) const {
  bool deep =
      this->state.getAliasStore().getAliasDepth() >= kMaxAliasDepth || calls > kMaxTailCalls;
  if (deep && !conditional) {
    std::string message = ErrorTextAliasDepth::endless(alias->name);

    return err(AliasInstantiatorDepthInBoundsError{
        .code = AliasInstantiatorDepthInBoundsErrorCode::ENDLESS_ALIAS,
        .message = message,
    });
  }
  if (deep && calls > kMaxTailCalls) {
    std::string message = ErrorTextAliasDepth::tailCalls(alias->name, kMaxTailCalls);

    return err(AliasInstantiatorDepthInBoundsError{
        .code = AliasInstantiatorDepthInBoundsErrorCode::TOO_MANY_TAIL_CALLS,
        .message = message,
    });
  }
  if (deep) {
    std::string message = ErrorTextAliasDepth::nests(alias->name, kMaxAliasDepth, kMaxTailCalls);

    return err(AliasInstantiatorDepthInBoundsError{
        .code = AliasInstantiatorDepthInBoundsErrorCode::TOO_DEEP,
        .message = message,
    });
  }

  return ok(calls);
}

std::vector<const Type *> AliasInstantiator::aliasTypeParams(TypeAliasDecl *alias) {
  AliasStore &aliases = this->state.getAliasStore();
  if (std::optional<AliasParams> known = aliases.findAliasParams(alias)) {
    return known->params;
  }
  this->state.getScopeStack().openTypeScope();
  std::vector<const Type *> defaults;
  std::vector<const Type *> params =
      this->typeParams->declareTypeParams(alias->typeParams, &defaults);
  this->state.getScopeStack().closeTypeScope();
  aliases.rememberAliasParams(alias, {std::move(params), std::move(defaults)});

  return aliases.findAliasParams(alias)->params;
}

const Type *AliasInstantiator::instantiateAlias(TypeAliasDecl *alias,
                                                std::vector<const Type *> args, llvm::SMLoc loc) {
  AliasStore &aliases = this->state.getAliasStore();
  if (aliases.getAliasDepth() == 0) {
    aliases.rememberAliasUse(loc);
  }
  AliasRun run{.alias = alias, .args = std::move(args), .loc = loc, .defaults = {}, .chain = {}};
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
  const Type *failed = this->failedAliasArgs(run);
  if (failed != nullptr) {
    return failed;
  }
  AliasInstance &entry = *this->state.getAliasStore().recordAliasInstance(run.alias, run.args);
  const Type *reused = AliasInstantiator::reusedInstance(entry);
  if (reused != nullptr) {
    return reused;
  }
  if (!this->aliasDepthFits(run.alias, entry, calls)) {
    return Type::error();
  }
  std::string display = AliasInstantiator::aliasDisplay(run);
  entry.resolving = true;
  run.chain.push_back(&entry);
  const Type *type = this->resolveAliasBody(run, entry, display);
  if (type == nullptr && this->state.getTypeContext().hasTailCall()) {
    TailCall tail = this->state.getTypeContext().takeTailCall();
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
  ResultArgumentCount counted =
      AliasInstantiator::argumentCount(run.alias, params.size(), run.args.size());
  if (counted.isErr()) {
    this->state.getDiagnostics().error(run.loc, counted.error().message);
    return Type::error();
  }
  for (size_t k = run.args.size(); k < params.size(); ++k) {
    ResultDefaultExists exists =
        AliasInstantiator::defaultExists(run.alias, params.size(), run.defaults[k]);
    if (exists.isErr()) {
      this->state.getDiagnostics().error(run.loc, exists.error().message);
      return Type::error();
    }
    run.args.push_back(this->typeParams->substitute(run.defaults[k], params, run.args));
  }
  for (size_t k = 0; k < params.size(); ++k) {
    if (run.args[k]->isError()) {
      return run.args[k];
    }
    ResultArgumentMatch matched = this->argumentMatch(run.alias, params, run.args, k);
    if (matched.isErr()) {
      this->state.getDiagnostics().error(run.loc, matched.error().message);
      return Type::error();
    }
  }

  return nullptr;
}

const Type *AliasInstantiator::reusedInstance(AliasInstance &entry) {
  if (entry.type != nullptr) {
    return entry.type;
  }
  if (entry.resolving && entry.self != nullptr) {
    entry.recursive = true;
    return entry.self;
  }

  return nullptr;
}

bool AliasInstantiator::aliasDepthFits(const TypeAliasDecl *alias, const AliasInstance &entry,
                                       unsigned calls) {
  ResultAliasDistinct distinct = AliasInstantiator::notResolving(alias, entry);
  if (distinct.isErr()) {
    this->state.getDiagnostics().error(alias->loc(), distinct.error().message);
    return false;
  }
  bool conditional = alias->type.kind() == TypeRefKind::CONDITIONAL_REF;
  ResultDepthInBounds bounded = this->depthInBounds(alias, calls, conditional);
  if (bounded.isOk()) {
    return true;
  }
  std::string neededHere = ErrorTextAliasDepth::neededHere();
  this->state.getDiagnostics().error(alias->loc(), bounded.error().message);
  llvm::SMLoc use = this->state.getAliasStore().getAliasUse();
  if (conditional && use.isValid() && use != alias->loc()) {
    this->state.getDiagnostics().note(use, neededHere);
  }

  return false;
}

std::string AliasInstantiator::aliasDisplay(const AliasRun &run) {
  const std::vector<const Type *> &args = run.args;
  const std::vector<const Type *> &defaults = run.defaults;
  size_t shown = args.size();
  while (shown > 0 && defaults[shown - 1] != nullptr &&
         (defaults[shown - 1] == args[shown - 1] ||
          (defaults[shown - 1]->isNever() && args[shown - 1]->isNever()))) {
    --shown;
  }
  if (shown == 0) {
    return run.alias->name.str();
  }
  std::string list = AliasInstantiator::typeList(llvm::ArrayRef(args).take_front(shown));

  return run.alias->name.str() + "<" + list + ">";
}

const Type *AliasInstantiator::resolveAliasBody(const AliasRun &run, AliasInstance &entry,
                                                const std::string &display) {
  TypeAliasDecl *alias = run.alias;
  this->state.getAliasStore().enterAlias();
  Module *home = this->state.getModuleStates().findDeclModule(alias);
  bool moved = home != nullptr && home != this->state.getModuleStates().getCurrentModule();
  if (moved) {
    this->state.getScopeStack().enterOnlyTypes(
        this->state.getModuleStates().getModuleState(home).types);
  }
  this->state.getScopeStack().openTypeScope();
  this->typeParams->bindTypeParams(alias->typeParams, run.args);
  const Type *type = alias->type.kind() == TypeRefKind::OBJECT_REF
                         ? this->objectAliasType(alias, entry, display)
                         : this->aliasTypeInner(alias);
  this->state.getScopeStack().closeTypeScope();
  if (moved) {
    this->state.getScopeStack().leaveOnlyTypes();
  }
  this->state.getAliasStore().leaveAlias();

  return type;
}

const Type *AliasInstantiator::objectAliasType(TypeAliasDecl *alias, AliasInstance &entry,
                                               const std::string &display) {
  entry.self = Type::declareObject(display);
  std::vector<const Type *> bases;
  std::vector<Overload> overloads;
  std::vector<Field> fields = alias->bases.empty()
                                  ? this->typeResolver->resolveFields(alias->type, &overloads)
                                  : this->classBases->interfaceFields(alias, bases, overloads);
  const Type *type = nullptr;
  if (entry.recursive) {
    entry.self->defineFields(std::move(fields), std::move(overloads));
    type = entry.self;
  } else {
    type = Type::object(std::move(fields), display, std::move(overloads));
  }
  for (const Type *base : bases) {
    if (!type->isError()) {
      this->classBases->addBase(type, base);
    }
  }

  return type;
}

const Type *AliasInstantiator::aliasTypeInner(const TypeAliasDecl *alias) {
  bool conditional = alias->type.kind() == TypeRefKind::CONDITIONAL_REF;
  if (conditional) {
    this->state.getTypeContext().allowTailCall();
  } else {
    this->state.getTypeContext().forbidTailCall();
  }
  const Type *type = this->typeResolver->resolveTypeInner(alias->type);
  this->state.getTypeContext().forbidTailCall();

  return type;
}

void AliasInstantiator::rememberAliasType(const AliasRun &run, const Type *type,
                                          const std::string &display) {
  AliasStore &aliases = this->state.getAliasStore();
  if (type->isObject() && !type->isError() && aliases.findAliasOrigin(type) == nullptr) {
    aliases.rememberAliasOrigin(type, run.alias, run.args);
  }
  if (type->isUnion() && aliases.findBuiltinAlias(run.alias->name) == run.alias) {
    Type::nameUnion(type, display);
  }
}
}
