#include "compiler/check/generics/alias_instantiator/alias_instantiator.h"
#include "compiler/check/classes/class_bases/class_bases.h"
#include "compiler/check/generics/generic_check_helpers/generic_check_helpers.h"
#include "compiler/check/generics/type_params/type_params.h"
#include "compiler/check/types/type_resolver/type_resolver.h"
#include "compiler/whole_program/flows/flows.h"
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

AliasInstantiator::AliasInstantiator(Checker &owner)
    : checker(owner) {}

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
  if (!this->checker.typeParams().satisfies(args[k], params[k], params, args)) {
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

AliasInstantiator::ResultAliasDistinct
AliasInstantiator::notResolving(const TypeAliasDecl *alias, const Checker::AliasInstance &entry) {
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
  bool deep = this->aliasDepth >= kMaxAliasDepth || calls > kMaxTailCalls;
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
  auto found = this->aliasParams.find(alias);
  if (found == this->aliasParams.end()) {
    this->checker.typeScopes_.emplace_back();
    std::vector<const Type *> defaults;
    std::vector<const Type *> params =
        this->checker.typeParams().declareTypeParams(alias->typeParams, &defaults);
    this->checker.typeScopes_.pop_back();
    found = this->aliasParams.try_emplace(alias, std::move(params), std::move(defaults)).first;
  }

  return found->second.first;
}

const Type *AliasInstantiator::instantiateAlias(TypeAliasDecl *alias,
                                                std::vector<const Type *> args, llvm::SMLoc loc) {
  std::vector<Checker::AliasInstance *> chain;
  if (this->aliasDepth == 0) {
    this->aliasUse = loc;
  }
  const Type *type = nullptr;
  for (unsigned calls = 0; !type; ++calls) {
    this->aliasTypeParams(alias);
    std::vector<const Type *> params = this->aliasParams[alias].first,
                              defaults = this->aliasParams[alias].second;
    ResultArgumentCount counted =
        AliasInstantiator::argumentCount(alias, params.size(), args.size());
    if (counted.isErr()) {
      this->checker.diags_.error(loc, counted.error().message);
      type = Type::error();
      break;
    }
    for (size_t k = args.size(); k < params.size() && !type; ++k) {
      ResultDefaultExists exists =
          AliasInstantiator::defaultExists(alias, params.size(), defaults[k]);
      if (exists.isErr()) {
        this->checker.diags_.error(loc, exists.error().message);
        type = Type::error();
        break;
      }
      args.push_back(this->checker.typeParams().substitute(defaults[k], params, args));
    }
    for (size_t k = 0; k < params.size() && !type; ++k) {
      if (args[k]->isError()) {
        type = args[k];
        break;
      }
      ResultArgumentMatch matched = this->argumentMatch(alias, params, args, k);
      if (matched.isErr()) {
        this->checker.diags_.error(loc, matched.error().message);
        type = Type::error();
      }
    }
    if (type) {
      break;
    }

    Checker::AliasInstance &entry = this->aliasInstances[{alias, args}];
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
      this->checker.diags_.error(alias->loc(), distinct.error().message);
      type = Type::error();
      break;
    }
    bool conditional = alias->type.kind() == TypeRefKind::CONDITIONAL_REF;
    ResultDepthInBounds bounded = this->depthInBounds(alias, calls, conditional);
    if (bounded.isErr()) {
      std::string neededHere = ErrorTextAliasDepth::neededHere();
      this->checker.diags_.error(alias->loc(), bounded.error().message);
      if (conditional && this->aliasUse.isValid() && this->aliasUse != alias->loc()) {
        this->checker.diags_.note(this->aliasUse, neededHere);
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
    ++this->aliasDepth;
    std::vector<llvm::StringMap<Stmt *>> outerTypes;
    Module *home = this->checker.declModules_.lookup(alias);
    bool moved = home && home != this->checker.module_;
    if (moved) {
      outerTypes = std::move(this->checker.typeScopes_);
      this->checker.typeScopes_ = {this->checker.state(home).types};
    }
    this->checker.typeScopes_.emplace_back();
    this->checker.typeParams().bindTypeParams(alias->typeParams, args);
    if (alias->type.kind() == TypeRefKind::OBJECT_REF) {
      entry.self = Type::declareObject(display);
      std::vector<const Type *> bases;
      std::vector<Overload> overloads;
      std::vector<Field> fields =
          alias->bases.empty()
              ? this->checker.typeResolver().resolveFields(alias->type, &overloads)
              : this->checker.classBases().interfaceFields(alias, bases, overloads);
      if (entry.recursive) {
        entry.self->defineFields(std::move(fields), std::move(overloads));
        type = entry.self;
      } else {
        type = Type::object(std::move(fields), display, std::move(overloads));
      }
      for (const Type *base : bases) {
        if (!type->isError()) {
          this->checker.classBases().addBase(type, base);
        }
      }
    } else {
      this->checker.tailAllowed_ = conditional;
      type = this->checker.typeResolver().resolveTypeInner(alias->type);
      this->checker.tailAllowed_ = false;
    }
    this->checker.typeScopes_.pop_back();
    if (moved) {
      this->checker.typeScopes_ = std::move(outerTypes);
    }
    --this->aliasDepth;
    if (!type && this->checker.tailCall_) {
      alias = this->checker.tailCall_->alias;
      args = std::move(this->checker.tailCall_->args);
      loc = alias->loc();
      this->checker.tailCall_.reset();
      continue;
    }
    if (!type) {
      type = Type::error();
    }
    if (type->isObject() && !type->isError() && !this->checker.aliasOrigins_.count(type)) {
      this->checker.aliasOrigins_[type] = {alias, args};
    }
    if (type->isUnion() && this->checker.builtinAliases_.lookup(alias->name) == alias) {
      Type::nameUnion(type, display);
    }
  }
  for (Checker::AliasInstance *entry : chain) {
    entry->resolving = false;
    entry->type = type;
  }

  return type;
}

}
