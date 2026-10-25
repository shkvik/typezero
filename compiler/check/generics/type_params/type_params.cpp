#include "compiler/check/generics/type_params/type_params.h"
#include "compiler/check/types/type_resolver/type_resolver.h"
#include "compiler/check/types/type_relations/type_relations.h"
#include "compiler/declare/class_declarer/class_declarer.h"
#include "compiler/check/classes/decorator_checker/decorator_checker.h"
#include "compiler/check/generics/alias_instantiator/alias_instantiator.h"
#include "compiler/check/generics/generic_class_instantiator/generic_class_instantiator.h"
#include "compiler/infer/return_inferrer/return_inferrer.h"
#include "compiler/check/types/type_operators/type_operators.h"
#include "compiler/check/generics/generic_check_helpers/generic_check_helpers.h"
#include "compiler/check/generics/inference/inference.h"
#include "compiler/whole_program/flows/flows.h"
#include "llvm/ADT/STLExtras.h"
#include <functional>
#include <set>

using llvm::cast;
using llvm::dyn_cast;
using llvm::isa;
using llvm::dyn_cast_or_null;

namespace tz {

namespace {

// Whether `type` holds an object type that mentions one of `params`: a
// class made from a generic one, an interface made from a generic one, a
// literal type. Objects of such types for T = User and for T = Post are
// told apart by their descriptors, unlike arrays of them.
bool mentionsInObject(const Type *type, llvm::ArrayRef<const Type *> params,
                      llvm::SmallPtrSetImpl<const Type *> &seen) {
  switch (type->kind()) {
  // `keyof T`, `T[K]`: of the very type T.
  case Type::Kind::Param:
    return type->typeOp() != TypeOp::None && mentionsAnyOf(type, params);
  case Type::Kind::Optional:
  case Type::Kind::Array:
    return mentionsInObject(type->inner(), params, seen);
  case Type::Kind::Result:
    return mentionsInObject(type->okType(), params, seen) ||
           mentionsInObject(type->errType(), params, seen);
  case Type::Kind::Function:
    return mentionsInObject(type->ret(), params, seen) ||
           llvm::any_of(type->params(),
                        [&](const Type *p) { return mentionsInObject(p, params, seen); });
  case Type::Kind::Iterator:
    return mentionsInObject(type->yielded(), params, seen) ||
           mentionsInObject(type->returned(), params, seen);
  case Type::Kind::Dict:
    return mentionsInObject(type->keyType(), params, seen) ||
           mentionsInObject(type->valueType(), params, seen);
  case Type::Kind::Union:
    return llvm::any_of(type->members(),
                        [&](const Type *m) { return mentionsInObject(m, params, seen); });
  case Type::Kind::Object:
    return !seen.contains(type) && mentionsAnyOf(type, params);
  default:
    return false;
  }
}

} // namespace

TypeParams::TypeParams(Sema &sema) : sema_(sema) {}

std::vector<const Type *> TypeParams::declareTypeParams(llvm::ArrayRef<TypeParamDecl> decls,
                                                  std::vector<const Type *> *defaults) {
  std::vector<Type *> created;
  for (size_t i = 0; i < decls.size(); ++i) {
    for (size_t j = 0; j < i; ++j) {
      if (decls[j].name == decls[i].name) {
        sema_.diags_.error(decls[i].loc, "duplicate type parameter '" + decls[i].name + "'");
      }
    }
    if (Type::fromName(decls[i].name)) {
      sema_.diags_.error(decls[i].loc, "'" + decls[i].name +
                                     "' is a built-in type and cannot name a type "
                                     "parameter");
    }
    created.push_back(Type::param(decls[i].name));
  }
  std::vector<const Type *> params(created.begin(), created.end());
  bindTypeParams(decls, params);

  bool afterDefault = false;
  for (size_t i = 0; i < decls.size(); ++i) {
    const TypeParamDecl &decl = decls[i];
    if (decl.constraint) {
      const TypeRef &ref = *decl.constraint;
      const Type *constraint = nullptr;
      std::vector<const Type *> alternatives;
      bool ok = true;
      if (ref.kind == TypeRef::Kind::Array && ref.args[0].kind == TypeRef::Kind::Name &&
          ref.args[0].name == "unknown") {
        // `A extends unknown[]`: the types of the parameters of some
        // function, a list of them (section 9); `unknown[]` means that only
        // here.
        created[i]->setList();
      } else if (ref.kind == TypeRef::Kind::Name && ref.name == "unknown") {
        // Any type at all, which is what no constraint says.
      } else if (ref.kind == TypeRef::Kind::Union) {
        // `T extends string | int`: an instance for each.
        for (const TypeRef &member : ref.args) {
          const Type *type = sema_.typeResolver().resolveType(member, /*allowVoid=*/false);
          ok &= !type->isError();
          if (!llvm::is_contained(alternatives, type)) {
            alternatives.push_back(type);
          }
        }
        // `T extends int | undefined` is one type.
        auto isUndefined = [](const Type *t) { return t->isUndefined(); };
        if (ok && alternatives.size() == 2 && llvm::any_of(alternatives, isUndefined)) {
          constraint = Type::optional(alternatives[alternatives[0]->isUndefined() ? 1 : 0]);
        }
      } else {
        constraint = sema_.typeResolver().resolveType(ref, /*allowVoid=*/false);
        ok &= !constraint->isError();
      }
      llvm::ArrayRef<const Type *> all = constraint ? llvm::ArrayRef<const Type *>(constraint)
                                                    : llvm::ArrayRef<const Type *>(alternatives);
      for (const Type *type : all) {
        if (!ok) {
          break;
        }
        if (type->isUndefined()) {
          sema_.diags_.error(ref.loc,
                       "the constraint of '" + decl.name + "' cannot be 'undefined' alone");
          ok = false;
        }
      }
      if (ok) {
        created[i]->setConstraint(constraint,
                                  constraint ? std::vector<const Type *>() : alternatives);
      }
    }
    const Type *fallback = nullptr;
    if (decl.defaultType) {
      fallback = sema_.typeResolver().resolveType(*decl.defaultType, /*allowVoid=*/false);
      if (!fallback->isError() && !satisfies(fallback, params[i])) {
        sema_.diags_.error(decl.defaultType->loc, "the default '" + fallback->name() + "' of '" +
                                                decl.name + "' does not satisfy its constraint '" +
                                                constraintSpelling(params[i], &decl) + "'");
        fallback = Type::error();
      }
      afterDefault = true;
    } else if (afterDefault) {
      sema_.diags_.error(decl.loc, "type parameter '" + decl.name +
                                 "' needs a default: it follows one that has one");
    }
    if (defaults) {
      defaults->push_back(fallback);
    }
  }

  return params;
}

void TypeParams::bindTypeParams(llvm::ArrayRef<TypeParamDecl> decls, llvm::ArrayRef<const Type *> types) {
  for (size_t i = 0; i < decls.size() && i < types.size(); ++i) {
    auto *alias = sema_.ctx_.make<TypeAliasDecl>(decls[i].loc, decls[i].name, TypeRef());
    alias->resolved = types[i];
    sema_.typeScopes_.back()[decls[i].name] = alias;
  }
}

const Type *TypeParams::substitute(const Type *type, llvm::ArrayRef<const Type *> params,
                             llvm::ArrayRef<const Type *> args) {
  if (!mentionsAnyOf(type, params)) {
    return type;
  }
  auto sub = [&](const Type *t) { return substitute(t, params, args); };
  switch (type->kind()) {
  case Type::Kind::Param: {
    // `T[K]` and the other computations, once their operands are known.
    if (type->typeOp() != TypeOp::None) {
      std::vector<const Type *> operands;
      for (const Type *t : type->operands()) {
        operands.push_back(t ? sub(t) : nullptr);
      }
      return sema_.typeOperators().evalTypeOp(type, operands);
    }
    const Type *arg = args[size_t(llvm::find(params, type) - params.begin())];
    return arg ? arg : type; // not known yet
  }
  case Type::Kind::Optional:
    return Type::optional(sub(type->inner()));
  case Type::Kind::Array:
    return Type::array(sub(type->inner()));
  case Type::Kind::Result:
    return Type::result(sub(type->okType()), sub(type->errType()));
  case Type::Kind::Function: {
    std::vector<const Type *> ps;
    for (const Type *p : type->params()) {
      ps.push_back(sub(p));
    }
    return Type::function(ps, type->minArgs(), type->hasRest(), sub(type->ret()), type->hasThis());
  }
  case Type::Kind::Iterator:
    return Type::iterator(sub(type->yielded()), sub(type->returned()), sub(type->sent()));
  case Type::Kind::Dict:
    return Type::dict(type->dictKind(), sub(type->keyType()), sub(type->valueType()));
  case Type::Kind::Union: {
    std::vector<const Type *> members;
    for (const Type *m : type->members()) {
      members.push_back(sub(m));
    }
    return Type::unionOf(members);
  }
  case Type::Kind::Promise:
    return Type::promise(sub(type->promised()));
  case Type::Kind::ClassRef: // `Constructor<T>`
    return type->isConstructor() ? Type::constructorOf(sub(type->constructed())) : type;
  case Type::Kind::Object: {
    if (type->isAsyncIterator()) {
      return Type::asyncIterator(sub(type->yielded()), sub(type->returned()), sub(type->sent()));
    }
    // `MethodDescriptor<A, R>` keeps its name.
    if (auto origin = sema_.descriptorOrigins_.find(type); origin != sema_.descriptorOrigins_.end()) {
      auto [list, ret] = origin->second;
      return sema_.decoratorChecker().methodDescriptorType(sub(list), sub(ret));
    }
    // A class made from a generic one is the instance for the other
    // arguments.
    if (const ClassDecl *cls = type->classDecl(); cls && !type->typeArgs().empty()) {
      std::vector<const Type *> newArgs;
      for (const Type *a : type->typeArgs()) {
        newArgs.push_back(sub(a));
      }
      // Where the instance is first needed in the program, if anywhere, is
      // where its errors are reported (see instantiateClass).
      auto *generic = const_cast<ClassDecl *>(cls->generic ? cls->generic : cls);
      ClassDecl *inst = sema_.genericClassInstantiator().instantiateClass(generic, std::move(newArgs), sema_.instanceLoc_);
      return inst ? inst->type : Type::error();
    }
    // An instance of a generic alias is that alias with other arguments; a
    // recursive one cannot be rebuilt field by field.
    if (auto origin = sema_.aliasOrigins_.find(type); origin != sema_.aliasOrigins_.end()) {
      // Copied: substituting may add entries.
      auto [alias, oldArgs] = origin->second;
      std::vector<const Type *> newArgs;
      for (const Type *a : oldArgs) {
        newArgs.push_back(sub(a));
      }
      return sema_.aliasInstantiator().instantiateAlias(alias, std::move(newArgs), alias->loc());
    }
    // `[int, T]` stays a tuple; `[int, ...A]` takes the types A stands for,
    // and an argument list keeps the elements that may be left out.
    if (type->isTuple()) {
      std::vector<const Type *> elements;
      std::vector<bool> spread;
      for (const Field &f : type->fields()) {
        elements.push_back(sub(f.type));
        spread.push_back(f.type->isListParam());
      }
      if (llvm::is_contained(spread, true)) {
        std::string why;
        const Type *list = spreadTuple(elements, spread, why);
        return list ? list : Type::error();
      }
      if (type->tupleMin() < elements.size() || type->tupleRest()) {
        return Type::argumentList(elements, type->tupleMin(),
                                  type->tupleRest() ? sub(type->tupleRest()) : nullptr);
      }
      return Type::tuple(elements, !type->fields().empty() && type->fields().front().readonly);
    }
    if (!substituting_.insert(type).second) {
      return type;
    }
    std::vector<Field> fields;
    for (const Field &f : type->fields()) {
      fields.push_back({f.name, sub(f.type), f.readonly});
    }
    substituting_.erase(type);
    return Type::object(std::move(fields));
  }
  default:
    return type;
  }
}

const Type *TypeParams::representation(const Type *type) {
  switch (type->kind()) {
  // An iterator and a dictionary are objects of the runtime, released
  // through their own descriptors as objects are.
  case Type::Kind::Object:
  case Type::Kind::Iterator:
  case Type::Kind::Dict:
    return Type::anyObject();
  case Type::Kind::Function:
    return Type::function({}, 0, false, Type::voidTy());
  case Type::Kind::Array:
    return Type::array(representation(type->inner()));
  case Type::Kind::Optional:
    return Type::optional(representation(type->inner()));
  case Type::Kind::Result: // its halves lie as such values do
    return Type::result(representation(type->okType()), representation(type->errType()));
  case Type::Kind::Union: {
    // The objects of a union are one reference; the tags of the other
    // members stay.
    std::vector<const Type *> members;
    for (const Type *m : type->members()) {
      members.push_back(representation(m));
    }
    return Type::unionOf(members);
  }
  default:
    return type;
  }
}

bool TypeParams::satisfies(const Type *arg, const Type *param, llvm::ArrayRef<const Type *> params,
                     llvm::ArrayRef<const Type *> args) {
  if (arg->isError()) {
    return true;
  }
  // `K extends keyof T`: with the type T stands for.
  const Type *own = param->constraint();
  if (own && !params.empty() && mentionsAnyOf(own, params)) {
    own = substitute(own, params, args);
  }
  if (own && own->isError()) {
    return true;
  }
  // A type parameter whose constraint is as strict.
  if (arg->isParam() && arg->constraint() && own &&
      TypeRelations::isImplicitlyConvertible(arg->constraint(), own)) {
    return true;
  }
  auto fits = [&](const Type *constraint) {
    return TypeRelations::isImplicitlyConvertible(arg, constraint) ||
           (constraint->isObject() && hasMembersOf(arg, constraint));
  };
  if (own) {
    return fits(own);
  }
  llvm::ArrayRef<const Type *> alternatives = param->alternatives();

  return alternatives.empty() || llvm::any_of(alternatives, fits);
}

bool TypeParams::hasMembersOf(const Type *arg, const Type *constraint) {
  if (arg->isParam() && arg->constraint()) {
    return hasMembersOf(arg->constraint(), constraint);
  }
  if (!arg->isObject() || arg->isTuple() != constraint->isTuple() || constraint->isClass() ||
      arg->isAnyObject()) {
    return false;
  }
  if (ClassDecl *cls = ClassDeclarer::classOf(arg)) {
    sema_.classDeclarer().completeClass(cls);
  }
  const ClassDecl *cls = arg->classDecl();
  for (const Field &f : constraint->fields()) {
    if (const Field *have = arg->field(f.name)) {
      // A read-only field of the constraint may be one that code changes.
      if (have->type != f.type || (have->readonly && !f.readonly)) {
        return false;
      }
      continue;
    }
    // A method of a class, for a field of the constraint that holds a
    // function and cannot be replaced, `greet(): string`.
    bool method = false;
    if (cls && f.readonly && f.type->isFunction()) {
      for (FunctionDecl *fn : ClassDeclarer::methodsNamed(cls, f.name, /*isStatic=*/false)) {
        method |= fn->sym && fn->typeParams.empty() && sema_.returnInferrer().functionType(fn->sym) == f.type;
      }
    }
    if (!method && !f.type->isOptional()) {
      return false;
    }
  }
  for (const Overload &overload : constraint->overloads()) {
    if (!cls) {
      return false;
    }
    std::vector<FunctionDecl *> methods = ClassDeclarer::methodsNamed(cls, overload.name, /*isStatic=*/false);
    for (const Type *signature : overload.signatures) {
      if (llvm::none_of(methods, [&](FunctionDecl *fn) {
            return fn->sym && sema_.returnInferrer().functionType(fn->sym) == signature;
          })) {
        return false;
      }
    }
  }

  return true;
}

bool TypeParams::exactTypeParam(FuncSymbol *generic, size_t k) {
  auto found = sema_.exactParams_.find(generic);
  if (found == sema_.exactParams_.end()) {
    llvm::ArrayRef<const Type *> params = generic->typeParams;
    std::vector<bool> exact(params.size(), false);
    for (size_t i = 0; i < params.size(); ++i) {
      // The body may read the fields of an object.
      auto objectLike = [](const Type *t) { return t && t->nonOptional()->isObject(); };
      exact[i] = objectLike(params[i]->constraint()) ||
                 llvm::any_of(params[i]->alternatives(), objectLike);
      // And iterates the type its own way: `xs: Iterable<int>`, and what it
      // gives, `xs: Iterable<T>`; a list of the types of parameters is the
      // parameters of the copy.
      exact[i] = exact[i] || params[i]->iterated() || params[i]->isListParam();
      for (const Type *p : params) {
        exact[i] = exact[i] || (p->iterated() && mentionsAnyOf(p->iterated(), params[i]));
      }
      // The keys of T: `K extends keyof T`, and `T[K]`, read the fields of
      // T by name.
      for (const Type *p : params) {
        exact[i] = exact[i] ||
                   (p != params[i] && p->constraint() && mentionsAnyOf(p->constraint(), params[i]));
      }
      // An instance of a generic class is a class of its own, and an
      // interface made of a generic one an interface of its own: a copy that
      // makes or takes `Box<T>` must make and take that of its own type.
      llvm::SmallPtrSet<const Type *, 4> seen;
      for (const Type *p : generic->params) {
        exact[i] = exact[i] || (p && mentionsInObject(p, params[i], seen));
      }
      if (generic->ret) {
        exact[i] = exact[i] || mentionsInObject(generic->ret, params[i], seen);
      }
    }
    found = sema_.exactParams_.try_emplace(generic, std::move(exact)).first;
  }

  return k < found->second.size() && found->second[k];
}

} // namespace tz
