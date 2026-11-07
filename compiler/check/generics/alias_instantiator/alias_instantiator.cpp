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
    if (args.size() > params.size()) {
      this->checker.diags_.error(
          loc, "'" + alias->name + "' takes " + std::to_string(params.size()) + " type argument" +
                   (params.size() == 1 ? "" : "s") + ", but got " + std::to_string(args.size()));
      type = Type::error();
      break;
    }
    for (size_t k = args.size(); k < params.size() && !type; ++k) {
      if (!defaults[k]) {
        this->checker.diags_.error(
            loc, "'" + alias->name + "' needs " + std::to_string(params.size()) + " type argument" +
                     (params.size() == 1 ? "" : "s") + ", e.g. '" + alias->name + "<int>'");
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
      if (!this->checker.typeParams().satisfies(args[k], params[k], params, args)) {
        this->checker.diags_.error(
            loc, "type '" + args[k]->name() + "' does not satisfy the constraint '" +
                     constraintSpelling(params[k], &alias->typeParams[k]) + "' of '" +
                     params[k]->name() + "' in '" + alias->name + "'");
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
    if (entry.resolving) {
      if (entry.self) {
        entry.recursive = true;
        type = entry.self;
        break;
      }
      this->checker.diags_.error(alias->loc(), "type alias '" + alias->name + "' refers to itself");
      type = Type::error();
      break;
    }
    bool conditional = alias->type.kind() == TypeRefKind::CONDITIONAL_REF;
    if (this->aliasDepth >= kMaxAliasDepth || calls > kMaxTailCalls) {
      if (!conditional) {
        this->checker.diags_.error(alias->loc(),
                                   "type alias '" + alias->name +
                                       "' refers to itself with other type arguments, "
                                       "so it never ends");
      } else if (calls > kMaxTailCalls) {
        this->checker.diags_.error(alias->loc(), "type alias '" + alias->name +
                                                     "' calls itself more than " +
                                                     std::to_string(kMaxTailCalls) +
                                                     " times, so it may never end; "
                                                     "TypeScript stops there too");
      } else {
        this->checker.diags_.error(
            alias->loc(),
            "type alias '" + alias->name + "' nests more than " + std::to_string(kMaxAliasDepth) +
                " instances of itself, so it may never "
                "end; TypeScript stops there too, and a branch that is the alias "
                "itself, '... ? " +
                alias->name + "<...> : ...', may go " + std::to_string(kMaxTailCalls) + " deep");
      }
      if (conditional && this->aliasUse.isValid() && this->aliasUse != alias->loc()) {
        this->checker.diags_.note(this->aliasUse, "the instance needed here");
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
