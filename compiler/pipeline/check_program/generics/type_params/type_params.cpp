#include "compiler/pipeline/check_program/generics/type_params/type_params.h"
#include "compiler/pipeline/check_program/declare/class_declarer/class_declarer.h"
#include "compiler/pipeline/check_program/generics/alias_instantiator/alias_instantiator.h"
#include "compiler/pipeline/check_program/generics/generic_check_helpers/generic_check_helpers.h"
#include "compiler/pipeline/check_program/generics/generic_class_instantiator/generic_class_instantiator.h"
#include "compiler/pipeline/check_program/generics/inference/inference.h"
#include "compiler/pipeline/check_program/infer/return_inferrer/return_inferrer.h"
#include "compiler/pipeline/check_program/topics/classes/decorator_checker/decorator_checker.h"
#include "compiler/pipeline/check_program/types/type_operators/type_operators.h"
#include "compiler/pipeline/check_program/types/type_relations/type_relations.h"
#include "compiler/pipeline/check_program/types/type_resolver/type_resolver.h"
#include "compiler/pipeline/check_program/whole_program/flows/flows.h"
#include "llvm/ADT/STLExtras.h"

namespace tz {

bool TypeParams::mentionsInObject(const Type *type, llvm::ArrayRef<const Type *> params,
                                  llvm::SmallPtrSetImpl<const Type *> &seen) {
  switch (type->kind()) {
  case Type::Kind::Param:
    return type->typeOp() != TypeOp::None && mentionsAnyOf(type, params);
  case Type::Kind::Optional:
  case Type::Kind::Array:
    return TypeParams::mentionsInObject(type->inner(), params, seen);
  case Type::Kind::Result:
    return TypeParams::mentionsInObject(type->okType(), params, seen) ||
           TypeParams::mentionsInObject(type->errType(), params, seen);
  case Type::Kind::Function:
    return TypeParams::mentionsInObject(type->ret(), params, seen) ||
           llvm::any_of(type->params(), [&](const Type *p) {
             return TypeParams::mentionsInObject(p, params, seen);
           });
  case Type::Kind::Iterator:
    return TypeParams::mentionsInObject(type->yielded(), params, seen) ||
           TypeParams::mentionsInObject(type->returned(), params, seen);
  case Type::Kind::Dict:
    return TypeParams::mentionsInObject(type->keyType(), params, seen) ||
           TypeParams::mentionsInObject(type->valueType(), params, seen);
  case Type::Kind::Union:
    return llvm::any_of(type->members(), [&](const Type *m) {
      return TypeParams::mentionsInObject(m, params, seen);
    });
  case Type::Kind::Object:
    return !seen.contains(type) && mentionsAnyOf(type, params);
  default:
    return false;
  }
}

TypeParams::TypeParams(CheckState &owner)
    : state(owner) {}

TypeParams::ResultParamNameIsNew TypeParams::uniqueName(const TypeParamDecl &earlier,
                                                        const TypeParamDecl &decl) {
  if (earlier.name == decl.name) {
    std::string message = ErrorTextUniqueParam::duplicate(decl.name);

    return err(TypeParamsUniqueNameError{
        .code = TypeParamsUniqueNameErrorCode::DUPLICATE_TYPE_PARAMETER,
        .message = message,
    });
  }

  return ok(&decl);
}

TypeParams::ResultNotBuiltInName TypeParams::ownName(const TypeParamDecl &decl) {
  if (Type::fromName(decl.name)) {
    std::string message = ErrorTextOwnName::builtin(decl.name);

    return err(TypeParamsOwnNameError{
        .code = TypeParamsOwnNameErrorCode::BUILTIN_NAME,
        .message = message,
    });
  }

  return ok(&decl);
}

TypeParams::ResultConstraintKept TypeParams::constraintAlone(const Type *type,
                                                             const TypeParamDecl &decl) {
  if (type->isUndefined()) {
    std::string message = ErrorTextConstraint::undefined(decl.name);

    return err(TypeParamsConstraintAloneError{
        .code = TypeParamsConstraintAloneErrorCode::UNDEFINED_CONSTRAINT,
        .message = message,
    });
  }

  return ok(type);
}

TypeParams::ResultDefaultSatisfy TypeParams::defaultFits(const Type *fallback, const Type *param,
                                                         const TypeParamDecl &decl) {
  if (!fallback->isError() && !this->satisfies(fallback, param)) {
    std::string constraint = constraintSpelling(param, &decl);
    std::string message =
        ErrorTextDefaultFits::unsatisfied(fallback->name(), decl.name, constraint);

    return err(TypeParamsDefaultFitsError{
        .code = TypeParamsDefaultFitsErrorCode::DEFAULT_UNSATISFIED,
        .message = message,
    });
  }

  return ok(fallback);
}

TypeParams::ResultDefaultPresent TypeParams::defaultGiven(const TypeParamDecl &decl,
                                                          bool afterDefault) {
  if (!decl.defaultType && afterDefault) {
    std::string message = ErrorTextDefaultGiven::missing(decl.name);

    return err(TypeParamsDefaultGivenError{
        .code = TypeParamsDefaultGivenErrorCode::MISSING_DEFAULT,
        .message = message,
    });
  }

  return ok(&decl);
}

std::vector<const Type *> TypeParams::declareTypeParams(llvm::ArrayRef<TypeParamDecl> decls,
                                                        std::vector<const Type *> *defaults) {
  std::vector<Type *> created;
  for (size_t i = 0; i < decls.size(); ++i) {
    for (size_t j = 0; j < i; ++j) {
      ResultParamNameIsNew unique = TypeParams::uniqueName(decls[j], decls[i]);
      if (unique.isErr()) {
        this->state.getDiagnostics().error(decls[i].loc, unique.error().message);
      }
    }
    ResultNotBuiltInName own = TypeParams::ownName(decls[i]);
    if (own.isErr()) {
      this->state.getDiagnostics().error(decls[i].loc, own.error().message);
    }
    created.push_back(Type::param(decls[i].name));
  }
  std::vector<const Type *> params(created.begin(), created.end());
  this->bindTypeParams(decls, params);

  bool afterDefault = false;
  for (size_t i = 0; i < decls.size(); ++i) {
    const TypeParamDecl &decl = decls[i];
    if (decl.constraint) {
      const TypeRef &ref = *decl.constraint;
      const Type *constraint = nullptr;
      std::vector<const Type *> alternatives;
      bool resolved = true;
      if (ref.kind() == TypeRefKind::ARRAY_REF && ref.args[0].kind() == TypeRefKind::NAME_REF &&
          ref.args[0].name == "unknown") {
        created[i]->setList();
      } else if (ref.kind() == TypeRefKind::NAME_REF && ref.name == "unknown") {
      } else if (ref.kind() == TypeRefKind::UNION_REF) {
        for (const TypeRef &member : ref.args) {
          const Type *type = TypeResolver(this->state).resolveType(member, false);
          resolved &= !type->isError();
          if (!llvm::is_contained(alternatives, type)) {
            alternatives.push_back(type);
          }
        }
        auto isUndefined = [](const Type *t) { return t->isUndefined(); };
        if (resolved && alternatives.size() == 2 && llvm::any_of(alternatives, isUndefined)) {
          constraint = Type::optional(alternatives[alternatives[0]->isUndefined() ? 1 : 0]);
        }
      } else {
        constraint = TypeResolver(this->state).resolveType(ref, false);
        resolved &= !constraint->isError();
      }
      llvm::ArrayRef<const Type *> all = constraint ? llvm::ArrayRef<const Type *>(constraint)
                                                    : llvm::ArrayRef<const Type *>(alternatives);
      for (const Type *type : all) {
        if (!resolved) {
          break;
        }
        ResultConstraintKept kept = TypeParams::constraintAlone(type, decl);
        if (kept.isErr()) {
          this->state.getDiagnostics().error(ref.loc, kept.error().message);
          resolved = false;
        }
      }
      if (resolved) {
        created[i]->setConstraint(constraint,
                                  constraint ? std::vector<const Type *>() : alternatives);
      }
    }
    const Type *fallback = nullptr;
    if (decl.defaultType) {
      fallback = TypeResolver(this->state).resolveType(*decl.defaultType, false);
      ResultDefaultSatisfy satisfied = this->defaultFits(fallback, params[i], decl);
      if (satisfied.isErr()) {
        this->state.getDiagnostics().error(decl.defaultType->loc, satisfied.error().message);
        fallback = Type::error();
      }
      afterDefault = true;
    }
    ResultDefaultPresent present = TypeParams::defaultGiven(decl, afterDefault);
    if (present.isErr()) {
      this->state.getDiagnostics().error(decl.loc, present.error().message);
    }
    if (defaults) {
      defaults->push_back(fallback);
    }
  }

  return params;
}

void TypeParams::bindTypeParams(llvm::ArrayRef<TypeParamDecl> decls,
                                llvm::ArrayRef<const Type *> types) {
  for (size_t i = 0; i < decls.size() && i < types.size(); ++i) {
    auto *alias = this->state.getAst().make<TypeAliasDecl>(decls[i].loc, decls[i].name, TypeRef());
    alias->resolved = types[i];
    alias->boundParam = true;
    this->state.getScopeStack().setType(decls[i].name, alias);
  }
}

const Type *TypeParams::substitute(const Type *type, llvm::ArrayRef<const Type *> params,
                                   llvm::ArrayRef<const Type *> args) {
  return this->substituteAt(type, params, args, true);
}

const Type *TypeParams::substituteValue(const Type *type, llvm::ArrayRef<const Type *> params,
                                        llvm::ArrayRef<const Type *> args) {
  return this->substituteAt(type, params, args, false);
}

const Type *TypeParams::substituteAt(const Type *type, llvm::ArrayRef<const Type *> params,
                                     llvm::ArrayRef<const Type *> args, bool voidOk) {
  if (!mentionsAnyOf(type, params)) {
    return type;
  }
  auto sub = [&](const Type *t) { return this->substituteAt(t, params, args, false); };
  auto held = [&](const Type *t) { return this->substituteAt(t, params, args, true); };
  switch (type->kind()) {
  case Type::Kind::Param: {
    if (type->typeOp() != TypeOp::None) {
      std::vector<const Type *> operands;
      for (const Type *t : type->operands()) {
        operands.push_back(t ? held(t) : nullptr);
      }
      return TypeOperators(this->state).evalTypeOp(type, operands);
    }
    const Type *arg = args[size_t(llvm::find(params, type) - params.begin())];
    if (arg && arg->isVoid() && !voidOk) {
      return Type::undefinedTy();
    }
    return arg ? arg : type;
  }
  case Type::Kind::Optional:
    return Type::optional(sub(type->inner()));
  case Type::Kind::Array:
    return Type::array(sub(type->inner()));
  case Type::Kind::Result:
    return Type::result(held(type->okType()), held(type->errType()));
  case Type::Kind::Function: {
    std::vector<const Type *> ps;
    for (const Type *p : type->params()) {
      ps.push_back(sub(p));
    }
    return Type::function(ps, type->minArgs(), type->hasRest(), held(type->ret()), type->hasThis());
  }
  case Type::Kind::Iterator:
    return Type::iterator(sub(type->yielded()), held(type->returned()), held(type->sent()));
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
    return Type::promise(held(type->promised()));
  case Type::Kind::ClassRef:
    return type->isConstructor() ? Type::constructorOf(sub(type->constructed())) : type;
  case Type::Kind::Object: {
    if (type->isAsyncIterator()) {
      return Type::asyncIterator(sub(type->yielded()), held(type->returned()), held(type->sent()));
    }
    if (const DescriptorOrigin *origin = this->state.getAliasStore().findDescriptorOrigin(type)) {
      auto [list, ret] = *origin;
      return DecoratorChecker(this->state).methodDescriptorType(sub(list), held(ret));
    }
    if (const ClassDecl *cls = type->classDecl(); cls && !type->typeArgs().empty()) {
      std::vector<const Type *> newArgs;
      for (const Type *a : type->typeArgs()) {
        newArgs.push_back(held(a));
      }
      auto *generic = const_cast<ClassDecl *>(cls->generic() ? cls->generic() : cls);
      GenericClassInstantiator::ResultClassMade made =
          GenericClassInstantiator(this->state)
              .instantiateClass(generic, std::move(newArgs),
                                this->state.getGenericStore().getInstanceLoc());
      if (made.isErr() && made.error().code !=
                              GenericClassInstantiatorInstantiateClassErrorCode::ALREADY_REPORTED) {
        llvm::SMLoc at =
            made.error().code ==
                    GenericClassInstantiatorInstantiateClassErrorCode::ENDLESS_INSTANCES
                ? generic->loc()
                : (this->state.getGenericStore().getInstanceLoc().isValid()
                       ? this->state.getGenericStore().getInstanceLoc()
                       : generic->loc());
        this->state.getDiagnostics().error(at, made.error().message);
      }
      return made.isOk() ? made.value()->type : Type::error();
    }
    if (const AliasOrigin *origin = this->state.getAliasStore().findAliasOrigin(type)) {
      auto [alias, oldArgs] = *origin;
      std::vector<const Type *> newArgs;
      for (const Type *a : oldArgs) {
        newArgs.push_back(held(a));
      }
      return AliasInstantiator(this->state)
          .instantiateAlias(alias, std::move(newArgs), alias->loc());
    }
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
    if (this->state.getGenericStore().isSubstituting(type)) {
      return type;
    }
    this->state.getGenericStore().enterSubstitution(type);
    std::vector<Field> fields;
    for (const Field &f : type->fields()) {
      fields.push_back({f.name, sub(f.type), f.readonly});
    }
    this->state.getGenericStore().leaveSubstitution(type);
    return Type::object(std::move(fields));
  }
  default:
    return type;
  }
}

const Type *TypeParams::representation(const Type *type) {
  switch (type->kind()) {
  case Type::Kind::Object:
  case Type::Kind::Iterator:
  case Type::Kind::Dict:
    return Type::anyObject();
  case Type::Kind::Function:
    return Type::function({}, 0, false, Type::voidTy());
  case Type::Kind::Array:
    return Type::array(TypeParams::representation(type->inner()));
  case Type::Kind::Optional:
    return Type::optional(TypeParams::representation(type->inner()));
  case Type::Kind::Result:
    return Type::result(TypeParams::representation(type->okType()),
                        TypeParams::representation(type->errType()));
  case Type::Kind::Union: {
    std::vector<const Type *> members;
    for (const Type *m : type->members()) {
      members.push_back(TypeParams::representation(m));
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
  const Type *own = param->constraint();
  if (own && !params.empty() && mentionsAnyOf(own, params)) {
    own = this->substitute(own, params, args);
  }
  if (own && own->isError()) {
    return true;
  }
  if (arg->isParam() && arg->constraint() && own &&
      TypeRelations::isImplicitlyConvertible(arg->constraint(), own)) {
    return true;
  }
  auto fits = [&](const Type *constraint) {
    return TypeRelations::isImplicitlyConvertible(arg, constraint) ||
           (constraint->isObject() && this->hasMembersOf(arg, constraint));
  };
  if (own) {
    return fits(own);
  }
  llvm::ArrayRef<const Type *> alternatives = param->alternatives();

  return alternatives.empty() || llvm::any_of(alternatives, fits);
}

bool TypeParams::hasMembersOf(const Type *arg, const Type *constraint) {
  if (arg->isParam() && arg->constraint()) {
    return this->hasMembersOf(arg->constraint(), constraint);
  }
  if (arg->isUnion()) {
    return llvm::all_of(arg->members(),
                        [&](const Type *m) { return this->hasMembersOf(m, constraint); });
  }
  if (!arg->isObject() || arg->isTuple() != constraint->isTuple() || constraint->isClass() ||
      arg->isAnyObject()) {
    return false;
  }
  if (ClassDecl *cls = ClassDeclarer::classOf(arg)) {
    ClassDeclarer(this->state).completeClass(cls);
  }
  const ClassDecl *cls = arg->classDecl();
  for (const Field &f : constraint->fields()) {
    if (const Field *have = arg->field(f.name)) {
      bool literal =
          f.type->isString() &&
          (have->type->isStringLiteral() ||
           (have->type->isUnion() && llvm::all_of(have->type->members(), [](const Type *m) {
              return m->isStringLiteral();
            })));
      if ((have->type != f.type && !literal) || (have->readonly && !f.readonly)) {
        return false;
      }
      continue;
    }
    bool method = false;
    if (cls && f.readonly && f.type->isFunction()) {
      for (FunctionDecl *fn : ClassDeclarer::methodsNamed(cls, f.name, false)) {
        method |= fn->sym && fn->typeParams.empty() &&
                  ReturnInferrer(this->state).functionType(fn->sym) == f.type;
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
    std::vector<FunctionDecl *> methods = ClassDeclarer::methodsNamed(cls, overload.name, false);
    for (const Type *signature : overload.signatures) {
      if (llvm::none_of(methods, [&](FunctionDecl *fn) {
            return fn->sym && ReturnInferrer(this->state).functionType(fn->sym) == signature;
          })) {
        return false;
      }
    }
  }

  return true;
}

bool TypeParams::exactTypeParam(FuncSymbol *generic, size_t k) {
  std::optional<std::vector<bool>> found = this->state.getGenericStore().findExactParams(generic);
  if (!found) {
    llvm::ArrayRef<const Type *> params = generic->typeParams;
    std::vector<bool> exact(params.size(), false);
    for (size_t i = 0; i < params.size(); ++i) {
      auto objectLike = [](const Type *t) { return t && t->nonOptional()->isObject(); };
      exact[i] = objectLike(params[i]->constraint()) ||
                 llvm::any_of(params[i]->alternatives(), objectLike);
      exact[i] = exact[i] || params[i]->iterated() || params[i]->isListParam();
      for (const Type *p : params) {
        exact[i] = exact[i] || (p->iterated() && mentionsAnyOf(p->iterated(), params[i]));
      }
      for (const Type *p : params) {
        exact[i] = exact[i] ||
                   (p != params[i] && p->constraint() && mentionsAnyOf(p->constraint(), params[i]));
      }
      llvm::SmallPtrSet<const Type *, 4> seen;
      for (const Type *p : generic->params) {
        exact[i] = exact[i] || (p && TypeParams::mentionsInObject(p, params[i], seen));
      }
      if (generic->ret) {
        exact[i] = exact[i] || TypeParams::mentionsInObject(generic->ret, params[i], seen);
      }
    }
    this->state.getGenericStore().rememberExactParams(generic, exact);
    found = std::move(exact);
  }

  return k < found->size() && (*found)[k];
}

}
