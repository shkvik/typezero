#include "compiler/pipeline/check_program/generics/alias_instantiator/alias_instantiator.h"
#include "compiler/pipeline/check_program/generics/generic_check_helpers/generic_check_helpers.h"
#include "compiler/pipeline/check_program/generics/type_params/type_params.h"
#include "compiler/pipeline/check_program/topics/classes/class_bases/class_bases.h"
#include "compiler/pipeline/check_program/types/type_resolver/type_resolver.h"
#include "compiler/pipeline/check_program/whole_program/flows/flows.h"
#include "llvm/ADT/STLExtras.h"

namespace tz {

namespace {

constexpr unsigned kMaxAliasDepth = 50;

constexpr unsigned kMaxTailCalls = 1000;

}

std::string AliasInstantiator::typeList(llvm::ArrayRef<const Type *> types) {
  std::string text;
  for (const Type *t : types) {
    text += (text.empty() ? "" : ", ") + t->name().str();
  }

  return text;
}

AliasInstantiator::AliasInstantiator(CheckState &owner)
    : state(owner) {}

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
  if (!TypeParams(this->state).satisfies(args[k], params[k], params, args)) {
    std::string constraint = constraintSpelling(params[k], &alias->typeParams[k]);
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
      TypeParams(this->state).declareTypeParams(alias->typeParams, &defaults);
  this->state.getScopeStack().closeTypeScope();
  aliases.rememberAliasParams(alias, {std::move(params), std::move(defaults)});

  return aliases.findAliasParams(alias)->params;
}

const Type *AliasInstantiator::instantiateAlias(TypeAliasDecl *alias,
                                                std::vector<const Type *> args, llvm::SMLoc loc) {
  std::vector<AliasInstance *> chain;
  AliasStore &aliases = this->state.getAliasStore();
  if (aliases.getAliasDepth() == 0) {
    aliases.rememberAliasUse(loc);
  }
  const Type *type = nullptr;
  for (unsigned calls = 0; !type; ++calls) {
    this->aliasTypeParams(alias);
    AliasParams declared = *aliases.findAliasParams(alias);
    std::vector<const Type *> params = declared.params;
    std::vector<const Type *> defaults = declared.defaults;
    ResultArgumentCount counted =
        AliasInstantiator::argumentCount(alias, params.size(), args.size());
    if (counted.isErr()) {
      this->state.getDiagnostics().error(loc, counted.error().message);
      type = Type::error();
      break;
    }
    for (size_t k = args.size(); k < params.size() && !type; ++k) {
      ResultDefaultExists exists =
          AliasInstantiator::defaultExists(alias, params.size(), defaults[k]);
      if (exists.isErr()) {
        this->state.getDiagnostics().error(loc, exists.error().message);
        type = Type::error();
        break;
      }
      args.push_back(TypeParams(this->state).substitute(defaults[k], params, args));
    }
    for (size_t k = 0; k < params.size() && !type; ++k) {
      if (args[k]->isError()) {
        type = args[k];
        break;
      }
      ResultArgumentMatch matched = this->argumentMatch(alias, params, args, k);
      if (matched.isErr()) {
        this->state.getDiagnostics().error(loc, matched.error().message);
        type = Type::error();
      }
    }
    if (type) {
      break;
    }

    AliasInstance &entry = *aliases.recordAliasInstance(alias, args);
    if (entry.type) {
      type = entry.type;
      break;
    }
    if (entry.resolving && entry.self) {
      entry.recursive = true;
      type = entry.self;
      break;
    }
    ResultAliasDistinct distinct = AliasInstantiator::notResolving(alias, entry);
    if (distinct.isErr()) {
      this->state.getDiagnostics().error(alias->loc(), distinct.error().message);
      type = Type::error();
      break;
    }
    bool conditional = alias->type.kind() == TypeRefKind::CONDITIONAL_REF;
    ResultDepthInBounds bounded = this->depthInBounds(alias, calls, conditional);
    if (bounded.isErr()) {
      std::string neededHere = ErrorTextAliasDepth::neededHere();
      this->state.getDiagnostics().error(alias->loc(), bounded.error().message);
      llvm::SMLoc use = aliases.getAliasUse();
      if (conditional && use.isValid() && use != alias->loc()) {
        this->state.getDiagnostics().note(use, neededHere);
      }
      type = Type::error();
      break;
    }
    size_t shown = args.size();
    while (shown > 0 && defaults[shown - 1] &&
           (defaults[shown - 1] == args[shown - 1] ||
            (defaults[shown - 1]->isNever() && args[shown - 1]->isNever()))) {
      --shown;
    }
    std::string display =
        shown == 0 ? alias->name.str()
                   : alias->name.str() + "<" +
                         AliasInstantiator::typeList(llvm::ArrayRef(args).take_front(shown)) + ">";
    entry.resolving = true;
    chain.push_back(&entry);
    aliases.enterAlias();
    Module *home = this->state.getModuleStates().findDeclModule(alias);
    bool moved = home && home != this->state.getModuleStates().getCurrentModule();
    if (moved) {
      this->state.getScopeStack().enterOnlyTypes(
          this->state.getModuleStates().getModuleState(home).types);
    }
    this->state.getScopeStack().openTypeScope();
    TypeParams(this->state).bindTypeParams(alias->typeParams, args);
    if (alias->type.kind() == TypeRefKind::OBJECT_REF) {
      entry.self = Type::declareObject(display);
      std::vector<const Type *> bases;
      std::vector<Overload> overloads;
      std::vector<Field> fields =
          alias->bases.empty() ? TypeResolver(this->state).resolveFields(alias->type, &overloads)
                               : ClassBases(this->state).interfaceFields(alias, bases, overloads);
      if (entry.recursive) {
        entry.self->defineFields(std::move(fields), std::move(overloads));
        type = entry.self;
      } else {
        type = Type::object(std::move(fields), display, std::move(overloads));
      }
      for (const Type *base : bases) {
        if (!type->isError()) {
          ClassBases(this->state).addBase(type, base);
        }
      }
    } else {
      if (conditional) {
        this->state.getTypeContext().allowTailCall();
      } else {
        this->state.getTypeContext().forbidTailCall();
      }
      type = TypeResolver(this->state).resolveTypeInner(alias->type);
      this->state.getTypeContext().forbidTailCall();
    }
    this->state.getScopeStack().closeTypeScope();
    if (moved) {
      this->state.getScopeStack().leaveOnlyTypes();
    }
    aliases.leaveAlias();
    if (!type && this->state.getTypeContext().hasTailCall()) {
      TailCall tail = this->state.getTypeContext().takeTailCall();
      alias = tail.alias;
      args = std::move(tail.args);
      loc = alias->loc();
      continue;
    }
    if (!type) {
      type = Type::error();
    }
    if (type->isObject() && !type->isError() && !aliases.findAliasOrigin(type)) {
      aliases.rememberAliasOrigin(type, alias, args);
    }
    if (type->isUnion() && aliases.findBuiltinAlias(alias->name) == alias) {
      Type::nameUnion(type, display);
    }
  }
  for (AliasInstance *entry : chain) {
    entry->resolving = false;
    entry->type = type;
  }

  return type;
}

}
