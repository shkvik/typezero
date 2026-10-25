#include "compiler/check/generics/alias_instantiator/alias_instantiator.h"
#include "compiler/check/classes/class_bases/class_bases.h"
#include "compiler/check/generics/generic_check_helpers/generic_check_helpers.h"
#include "compiler/check/generics/type_params/type_params.h"
#include "compiler/check/types/type_resolver/type_resolver.h"
#include "compiler/whole_program/flows/flows.h"
#include "llvm/ADT/STLExtras.h"
#include <functional>
#include <set>

using llvm::cast;
using llvm::dyn_cast;
using llvm::dyn_cast_or_null;
using llvm::isa;

namespace tz {

namespace {

// How deep instances of generic aliases may nest, each needed by the one
// that resolves it, as in TypeScript: an alias that refers to itself with
// other arguments, or a conditional type whose branch does. A conditional
// type that ends in another instance, a tail call, goes on in a loop
// instead, up to kMaxTailCalls times (TypeScript's limits too).
constexpr unsigned kMaxAliasDepth = 50;

constexpr unsigned kMaxTailCalls = 1000;

// "int, string"
std::string typeList(llvm::ArrayRef<const Type *> types) {
  std::string text;
  for (const Type *t : types) {
    text += (text.empty() ? "" : ", ") + t->name().str();
  }

  return text;
}

} // namespace

AliasInstantiator::AliasInstantiator(Checker &checker) : checker_(checker) {}

std::vector<const Type *> AliasInstantiator::aliasTypeParams(TypeAliasDecl *alias) {
  auto found = aliasParams_.find(alias);
  if (found == aliasParams_.end()) {
    checker_.typeScopes_.emplace_back();
    std::vector<const Type *> defaults;
    std::vector<const Type *> params =
        checker_.typeParams().declareTypeParams(alias->typeParams, &defaults);
    checker_.typeScopes_.pop_back();
    found = aliasParams_.try_emplace(alias, std::move(params), std::move(defaults)).first;
  }

  return found->second.first;
}

const Type *AliasInstantiator::instantiateAlias(TypeAliasDecl *alias,
                                                std::vector<const Type *> args, llvm::SMLoc loc) {
  // The instances that tail calls went through: the type is theirs too.
  std::vector<Checker::AliasInstance *> chain;
  // Where the program needs the outermost instance, for an endless one.
  if (aliasDepth_ == 0) {
    aliasUse_ = loc;
  }
  const Type *type = nullptr;
  for (unsigned calls = 0; !type; ++calls) {
    aliasTypeParams(alias);
    // Copied: instantiating others below may move the map's entries.
    std::vector<const Type *> params = aliasParams_[alias].first,
                              defaults = aliasParams_[alias].second;
    if (args.size() > params.size()) {
      checker_.diags_.error(loc, "'" + alias->name + "' takes " + std::to_string(params.size()) +
                                     " type argument" + (params.size() == 1 ? "" : "s") +
                                     ", but got " + std::to_string(args.size()));
      type = Type::error();
      break;
    }
    for (size_t k = args.size(); k < params.size() && !type; ++k) {
      if (!defaults[k]) {
        checker_.diags_.error(loc, "'" + alias->name + "' needs " + std::to_string(params.size()) +
                                       " type argument" + (params.size() == 1 ? "" : "s") +
                                       ", e.g. '" + alias->name + "<int>'");
        type = Type::error();
        break;
      }
      args.push_back(checker_.typeParams().substitute(defaults[k], params, args));
    }
    for (size_t k = 0; k < params.size() && !type; ++k) {
      if (args[k]->isError()) {
        type = args[k];
        break;
      }
      if (!checker_.typeParams().satisfies(args[k], params[k], params, args)) {
        checker_.diags_.error(loc, "type '" + args[k]->name() +
                                       "' does not satisfy the constraint '" +
                                       constraintSpelling(params[k], &alias->typeParams[k]) +
                                       "' of '" + params[k]->name() + "' in '" + alias->name + "'");
        type = Type::error();
      }
    }
    if (type) {
      break;
    }

    // A std::map: the entry stays where it is while others are added.
    Checker::AliasInstance &entry = aliasInstances_[{alias, args}];
    if (entry.type) {
      type = entry.type;
      break;
    }
    if (entry.resolving) {
      // An object type may refer to itself through its fields.
      if (entry.self) {
        entry.recursive = true;
        type = entry.self;
        break;
      }
      checker_.diags_.error(alias->loc(), "type alias '" + alias->name + "' refers to itself");
      type = Type::error();
      break;
    }
    bool conditional = alias->type.kind == TypeRef::Kind::Conditional;
    if (aliasDepth_ >= kMaxAliasDepth || calls > kMaxTailCalls) {
      if (!conditional) {
        checker_.diags_.error(alias->loc(), "type alias '" + alias->name +
                                                "' refers to itself with other type arguments, "
                                                "so it never ends");
      } else if (calls > kMaxTailCalls) {
        checker_.diags_.error(alias->loc(), "type alias '" + alias->name +
                                                "' calls itself more than " +
                                                std::to_string(kMaxTailCalls) +
                                                " times, so it may never end; "
                                                "TypeScript stops there too");
      } else {
        checker_.diags_.error(
            alias->loc(),
            "type alias '" + alias->name + "' nests more than " + std::to_string(kMaxAliasDepth) +
                " instances of itself, so it may never "
                "end; TypeScript stops there too, and a branch that is the alias "
                "itself, '... ? " +
                alias->name + "<...> : ...', may go " + std::to_string(kMaxTailCalls) + " deep");
      }
      if (conditional && aliasUse_.isValid() && aliasUse_ != alias->loc()) {
        checker_.diags_.note(aliasUse_, "the instance needed here");
      }
      type = Type::error();
      break;
    }
    // `Pair<int>` in messages.
    std::string display = alias->name.str() + "<" + typeList(args) + ">";
    entry.resolving = true;
    chain.push_back(&entry);
    ++aliasDepth_;
    // The names it uses mean what they mean in its own file, which may be
    // another one: `interface Source<R> { pull?: (c: Controller<R>) => void }`
    // of a file that imports only Source.
    std::vector<llvm::StringMap<Stmt *>> outerTypes;
    Module *home = checker_.declModules_.lookup(alias);
    bool moved = home && home != checker_.module_;
    if (moved) {
      outerTypes = std::move(checker_.typeScopes_);
      checker_.typeScopes_ = {checker_.state(home).types};
    }
    checker_.typeScopes_.emplace_back();
    checker_.typeParams().bindTypeParams(alias->typeParams, args);
    if (alias->type.kind == TypeRef::Kind::Object) {
      entry.self = Type::declareObject(display);
      // A generic interface, `interface Repo<T> extends Named { ... }`, as
      // resolveAlias makes one.
      std::vector<const Type *> bases;
      std::vector<Overload> overloads;
      std::vector<Field> fields =
          alias->bases.empty() ? checker_.typeResolver().resolveFields(alias->type, &overloads)
                               : checker_.classBases().interfaceFields(alias, bases, overloads);
      if (entry.recursive) {
        entry.self->defineFields(std::move(fields), std::move(overloads));
        type = entry.self;
      } else {
        type = Type::object(std::move(fields), display, std::move(overloads));
      }
      for (const Type *base : bases) {
        if (!type->isError()) {
          checker_.classBases().addBase(type, base);
        }
      }
    } else {
      checker_.tailAllowed_ = conditional;
      type = checker_.typeResolver().resolveTypeInner(alias->type);
      checker_.tailAllowed_ = false;
    }
    checker_.typeScopes_.pop_back();
    if (moved) {
      checker_.typeScopes_ = std::move(outerTypes);
    }
    --aliasDepth_;
    // The conditional type ended in another instance: that one next.
    if (!type && checker_.tailCall_) {
      alias = checker_.tailCall_->alias;
      args = std::move(checker_.tailCall_->args);
      loc = alias->loc();
      checker_.tailCall_.reset();
      continue;
    }
    if (!type) {
      type = Type::error();
    }
    if (type->isObject() && !type->isError() && !checker_.aliasOrigins_.count(type)) {
      checker_.aliasOrigins_[type] = {alias, args};
    }
  }
  for (Checker::AliasInstance *entry : chain) {
    entry->resolving = false;
    entry->type = type;
  }

  return type;
}

} // namespace tz
