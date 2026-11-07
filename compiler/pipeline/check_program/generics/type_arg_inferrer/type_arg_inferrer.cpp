#include "compiler/pipeline/check_program/generics/type_arg_inferrer/type_arg_inferrer.h"
#include "compiler/pipeline/check_program/generics/arg_list_checker/arg_list_checker.h"
#include "compiler/pipeline/check_program/generics/generic_check_helpers/generic_check_helpers.h"
#include "compiler/pipeline/check_program/generics/generic_checker/generic_checker.h"
#include "compiler/pipeline/check_program/generics/generic_instantiator/generic_instantiator.h"
#include "compiler/pipeline/check_program/generics/inference/inference.h"
#include "compiler/pipeline/check_program/generics/type_params/type_params.h"
#include "compiler/pipeline/check_program/infer/return_inferrer/return_inferrer.h"
#include "compiler/pipeline/check_program/instances/late_classes/late_classes.h"
#include "compiler/pipeline/check_program/topics/expr/argument_checker/argument_checker.h"
#include "compiler/pipeline/check_program/topics/expr/call_checker/call_checker.h"
#include "compiler/pipeline/check_program/topics/expr/expr_checker/expr_checker.h"
#include "compiler/pipeline/check_program/topics/functions/closure_checker/closure_checker.h"
#include "compiler/pipeline/check_program/topics/functions/function_value_checker/function_value_checker.h"
#include "compiler/pipeline/check_program/topics/iterators/iterable_checker/iterable_checker.h"
#include "compiler/pipeline/check_program/topics/result/result_types/result_types.h"
#include "compiler/pipeline/check_program/types/literal_types/literal_types.h"
#include "compiler/pipeline/check_program/types/type_relations/type_relations.h"
#include "compiler/pipeline/check_program/types/type_resolver/type_resolver.h"
#include "compiler/pipeline/check_program/types/utility_types/utility_types.h"
#include "llvm/ADT/STLExtras.h"

using llvm::cast;
using llvm::dyn_cast;
using llvm::isa;

namespace tz {

bool TypeArgInferrer::grows(const Type *type, llvm::ArrayRef<const Type *> params, bool inside) {
  switch (type->kind()) {
  case Type::Kind::Param:
    return inside && llvm::is_contained(params, type);
  case Type::Kind::Optional:
    return TypeArgInferrer::grows(type->inner(), params, inside);
  case Type::Kind::Array:
  case Type::Kind::Promise:
    return TypeArgInferrer::grows(type->inner(), params, true);
  case Type::Kind::Result:
    return TypeArgInferrer::grows(type->okType(), params, true) ||
           TypeArgInferrer::grows(type->errType(), params, true);
  case Type::Kind::Function:
    return TypeArgInferrer::grows(type->ret(), params, true) ||
           llvm::any_of(type->params(),
                        [&](const Type *p) { return TypeArgInferrer::grows(p, params, true); });
  case Type::Kind::Object:
  case Type::Kind::Iterator:
  case Type::Kind::Dict:
    return GenericCheckHelpers::mentionsAnyOf(type, params);
  default:
    return false;
  }
}

bool TypeArgInferrer::isContextSensitive(const Expr *arg) {
  if (auto *fe = dyn_cast<FunctionExpr>(arg)) {
    return llvm::any_of(fe->fn->params, [](const Param &p) { return !p.type; });
  }
  if (auto *lit = dyn_cast<ArrayLitExpr>(arg)) {
    return llvm::any_of(lit->elements, [](const ArrayLitExpr::Element &e) {
      return TypeArgInferrer::isContextSensitive(e.expr);
    });
  }
  if (auto *lit = dyn_cast<ObjectLitExpr>(arg)) {
    return llvm::any_of(lit->props, [](const ObjectLitExpr::Property &p) {
      return TypeArgInferrer::isContextSensitive(p.value);
    });
  }
  if (auto *name = dyn_cast<NameExpr>(arg)) {
    return name->typeArgs.empty() &&
           llvm::is_contained(llvm::ArrayRef<llvm::StringRef>{"String", "Number", "parseInt",
                                                              "parseFloat", "Boolean",
                                                              "encodeURIComponent", "encodeURI",
                                                              "decodeURIComponent", "decodeURI"},
                              name->name);
  }
  if (auto *member = dyn_cast<MemberExpr>(arg)) {
    if (auto *object = dyn_cast<NameExpr>(member->object)) {
      return object->name == "Math" || object->name == "Number" || object->name == "String" ||
             object->name == "console";
    }
  }

  return false;
}

TypeArgInferrer::TypeArgInferrer(CheckState &owner)
    : state(owner) {}

void TypeArgInferrer::precheckFields(ObjectLitExpr *lit, const Type *pattern,
                                     llvm::ArrayRef<const Type *> typeParams,
                                     Inference &inference) {
  const Type *target = pattern->nonOptional();
  if (!target->isObject() || target->isClass() || target->isTuple() || lit->hasSpread() ||
      llvm::any_of(lit->props, [](const ObjectLitExpr::Property &p) {
        return p.method() || p.key() || p.defaulted();
      })) {
    return;
  }
  for (ObjectLitExpr::Property &prop : lit->props) {
    const Field *field = target->field(prop.name);
    if (!field || !GenericCheckHelpers::mentionsAnyOf(field->type, typeParams) ||
        TypeArgInferrer::isContextSensitive(prop.value) || isa<ObjectLitExpr>(prop.value) ||
        isa<ArrayLitExpr>(prop.value) || GenericChecker::literalShape(prop.value)) {
      continue;
    }
    prop.value = ExprChecker(this->state).checkExpr(prop.value);
    this->state.getExpressionMarks().markPrechecked(prop.value);
    inference.from(field->type, prop.value->type);
  }
}

TypeArgInferrer::ResultParamInferred
TypeArgInferrer::paramInferred(const FuncSymbol *generic, const Type *bound, size_t k, bool call) {
  llvm::ArrayRef<const Type *> params = generic->typeParams;
  if (!bound) {
    std::string ints;
    for (size_t i = 0; i < params.size(); ++i) {
      ints += (i == 0 ? "" : ", ") + std::string(params[i]->isListParam() ? "[int]" : "int");
    }
    std::string message =
        ErrorTextParamInferred::cannot(params[k]->name(), generic->name, call, ints);

    return err(TypeArgInferrerParamInferredError{
        .code = TypeArgInferrerParamInferredErrorCode::CANNOT_INFER,
        .message = message,
    });
  }

  return ok(bound);
}

TypeArgInferrer::ResultIterableMatch TypeArgInferrer::iterableFits(const FuncSymbol *generic,
                                                                   const Type *bound,
                                                                   const Type *given,
                                                                   const Type *wanted) {
  bool fits = given == wanted || wanted->isError() || bound->isError();
  if (!fits && given) {
    std::string message = ErrorTextIterableFits::elements(bound->name(), given->name(),
                                                          wanted->name(), generic->name);

    return err(TypeArgInferrerIterableFitsError{
        .code = TypeArgInferrerIterableFitsErrorCode::WRONG_ELEMENTS,
        .message = message,
    });
  }
  if (!fits) {
    std::string message =
        ErrorTextIterableFits::notIter(bound->name(), wanted->name(), generic->name);

    return err(TypeArgInferrerIterableFitsError{
        .code = TypeArgInferrerIterableFitsErrorCode::NOT_ITERABLE,
        .message = message,
    });
  }

  return ok(bound);
}

TypeArgInferrer::ResultConstraintMet
TypeArgInferrer::constraintMet(const FuncSymbol *generic, std::vector<const Type *> &bound,
                               size_t k) {
  llvm::ArrayRef<const Type *> params = generic->typeParams;
  if (!TypeParams(this->state).satisfies(bound[k], params[k], params, bound)) {
    llvm::ArrayRef<TypeParamDecl> decls = generic->decl->typeParams;
    std::string constraint =
        GenericCheckHelpers::constraintSpelling(params[k], k < decls.size() ? &decls[k] : nullptr);
    std::string message = ErrorTextConstraintMet::unsatisfied(bound[k]->name(), constraint,
                                                              params[k]->name(), generic->name);

    return err(TypeArgInferrerConstraintMetError{
        .code = TypeArgInferrerConstraintMetErrorCode::CONSTRAINT_UNSATISFIED,
        .message = message,
    });
  }

  return ok(bound[k]);
}

TypeArgInferrer::ResultTypeArgsCount
TypeArgInferrer::typeArgCount(const FuncSymbol *generic, llvm::ArrayRef<TypeRef> typeArgs) {
  if (typeArgs.size() > generic->typeParams.size()) {
    std::string message =
        ErrorTextTypeArgCount::tooMany(generic->name, generic->typeParams.size(), typeArgs.size());

    return err(TypeArgInferrerTypeArgCountError{
        .code = TypeArgInferrerTypeArgCountErrorCode::TOO_MANY_TYPE_ARGUMENTS,
        .message = message,
    });
  }

  return ok(typeArgs.size());
}

TypeArgInferrer::ResultCandidateJoin TypeArgInferrer::candidatesJoin(const FuncSymbol *generic,
                                                                     const Type *param,
                                                                     const Type *type,
                                                                     const Type *candidate) {
  const Type *next = type ? TypeRelations::unify(type, candidate) : candidate;
  if (!next) {
    std::string message = ErrorTextCandidates::conflict(param->name(), generic->name, type->name(),
                                                        candidate->name());

    return err(TypeArgInferrerCandidatesJoinError{
        .code = TypeArgInferrerCandidatesJoinErrorCode::CONFLICTING_CANDIDATES,
        .message = message,
    });
  }

  return ok(next);
}

TypeArgInferrer::ResultCallbackParam
TypeArgInferrer::callbackParam(const FuncSymbol *generic, const Type *param, const Type *bound,
                               bool mentioned, const FunctionExpr *fe, size_t p) {
  if (!bound && mentioned) {
    std::string what = fe && p < fe->fn->params.size()
                           ? ErrorTextCallbackParam::param(fe->fn->params[p].name)
                           : ErrorTextCallbackParam::params();
    std::string message =
        ErrorTextCallbackParam::uninferred(param->name(), generic->name, what, fe != nullptr);

    return err(TypeArgInferrerCallbackParamError{
        .code = TypeArgInferrerCallbackParamErrorCode::CALLBACK_PARAM_UNINFERRED,
        .message = message,
    });
  }

  return ok(param);
}

TypeArgInferrer::ResultArgNotGrowing
TypeArgInferrer::notGrowing(const FuncSymbol *generic, const Type *bound, const Type *param) {
  if (TypeArgInferrer::grows(bound, generic->typeParams, false)) {
    std::string message = ErrorTextNotGrowing::grows(generic->name, bound->name(), param->name());

    return err(TypeArgInferrerNotGrowingError{
        .code = TypeArgInferrerNotGrowingErrorCode::GROWING_TYPE_ARGUMENT,
        .message = message,
    });
  }

  return ok(bound);
}

bool TypeArgInferrer::finishTypeArgs(FuncSymbol *generic, std::vector<const Type *> &bound,
                                     llvm::SMLoc loc, bool call, bool quiet) {
  llvm::ArrayRef<const Type *> params = generic->typeParams;
  bool inferred = true;
  for (size_t k = 0; k < params.size(); ++k) {
    if (!bound[k] && k < generic->typeDefaults.size() && generic->typeDefaults[k]) {
      const Type *fallback =
          TypeParams(this->state).substitute(generic->typeDefaults[k], params, bound);
      bool open = false;
      for (size_t j = 0; j < params.size(); ++j) {
        open |= !bound[j] && GenericCheckHelpers::mentionsAnyOf(fallback, params[j]);
      }
      if (!open) {
        bound[k] = fallback;
      }
    }
    if (bound[k] && bound[k]->isError()) {
      inferred = false;
      continue;
    }
    if (!bound[k] && quiet) {
      bound[k] = Type::error();
      inferred = false;
      continue;
    }
    ResultParamInferred paramKnown = TypeArgInferrer::paramInferred(generic, bound[k], k, call);
    if (paramKnown.isErr()) {
      this->state.getDiagnostics().error(loc, paramKnown.error().message);
      bound[k] = Type::error();
      inferred = false;
      continue;
    }
    if (const Type *element = params[k]->iterated()) {
      const Type *wanted = TypeParams(this->state).substitute(element, params, bound);
      const Type *elements = IterableChecker(this->state).elementTypeOf(bound[k]);
      ResultIterableMatch matched =
          TypeArgInferrer::iterableFits(generic, bound[k], elements, wanted);
      if (matched.isErr()) {
        this->state.getDiagnostics().error(loc, matched.error().message);
        bound[k] = Type::error();
        inferred = false;
      }
      continue;
    }
    ResultConstraintMet met = this->constraintMet(generic, bound, k);
    if (met.isErr()) {
      this->state.getDiagnostics().error(loc, met.error().message);
      bound[k] = Type::error();
      inferred = false;
    }
  }

  return inferred;
}

Expr *TypeArgInferrer::checkGenericCall(CallExpr *call, FuncSymbol *generic, llvm::SMLoc loc,
                                        const Type *expected, llvm::ArrayRef<TypeRef> typeArgs) {
  std::vector<const Type *> bound;
  if (!this->bindGenericCall(call, generic, loc, expected, typeArgs, bound)) {
    return call;
  }
  call->target = FunctionCall{.func = generic};
  const ClassDecl *cls = generic->decl ? generic->decl->cls() : nullptr;
  bool concrete =
      llvm::none_of(bound, [](const Type *t) { return TypeQueries::mentionsParam(t); }) &&
      (!cls || generic->decl->isStatic() || cls->isEmitted());
  bool copiesReturn = LateClasses(this->state).isTemplate(generic) && !generic->ret;
  if (concrete && !this->state.getFunctionStack().isInsideTrial() && !generic->inferring &&
      (generic->ret || copiesReturn)) {
    if (FuncSymbol *instance =
            GenericInstantiator(this->state)
                .instantiate(generic, bound, loc,
                             GenericInstantiator(this->state).exactClassArgs(call, generic))) {
      call->target = FunctionCall{.func = instance};
      if (copiesReturn) {
        call->type = instance->ret ? instance->ret : Type::error();
      }
    }
  }

  return call;
}

bool TypeArgInferrer::bindGenericCall(CallExpr *call, FuncSymbol *generic, llvm::SMLoc loc,
                                      const Type *expected, llvm::ArrayRef<TypeRef> typeArgs,
                                      std::vector<const Type *> &bound) {
  call->type = Type::error();
  unsigned errorsBefore = this->state.getDiagnostics().errorsSeen();
  ReturnInferrer(this->state).resolveParams(generic);
  const Type *ret = ReturnInferrer(this->state).returnType(generic);
  if (GenericInstantiator(this->state).returnsOwnClass(generic, ret)) {
    ret = nullptr;
  }
  llvm::ArrayRef<const Type *> typeParams = generic->typeParams;
  size_t fixed = generic->params.size() - (generic->hasRest ? 1 : 0);
  for (size_t i = 0; i < call->args.size() && i < (generic->hasRest ? fixed : call->args.size());
       ++i) {
    if (call->spreads(i) && CallChecker(this->state).rejectSpread(call)) {
      return false;
    }
  }
  ArgumentChecker::ResultArityMatches arity =
      ArgumentChecker(this->state)
          .checkArity(call, static_cast<unsigned>(generic->minArgs),
                      generic->hasRest ? ~0u : static_cast<unsigned>(generic->params.size()));
  if (arity.isErr()) {
    this->state.getDiagnostics().error(loc, arity.error().message);
    return false;
  }
  auto paramOf = [&](size_t i) -> const Type * {
    if (!generic->hasRest || i < fixed) {
      return generic->params[i];
    }
    const Type *rest = generic->params.back();

    return call->spreads(i) || !rest->isArray() ? rest : rest->inner();
  };
  const Type *listRest = generic->hasRest && (generic->params.back()->isListParam() ||
                                              ArgListChecker::spreadsLists(generic->params.back()))
                             ? generic->params.back()
                             : nullptr;

  bound.assign(typeParams.size(), nullptr);
  std::vector<bool> checked(call->args.size(), false);
  std::vector<size_t> literals;
  bool inferred = true;
  if (!typeArgs.empty()) {
    ResultTypeArgsCount counted = TypeArgInferrer::typeArgCount(generic, typeArgs);
    if (counted.isErr()) {
      this->state.getDiagnostics().error(loc, counted.error().message);
      inferred = false;
    }
    for (size_t k = 0; k < typeArgs.size() && k < typeParams.size(); ++k) {
      bound[k] = TypeResolver(this->state).resolveType(typeArgs[k], true);
    }
  } else {
    Inference inference(typeParams, this->state.getAliasStore(), [this](const Type *t) {
      return IterableChecker(this->state).elementTypeOf(t);
    });
    inference.asyncOf = [this](const Type *t) {
      return IterableChecker(this->state).asyncIterableType(t);
    };
    auto settle = [&](size_t k) {
      if (bound[k]) {
        return;
      }
      const Type *type = nullptr;
      bool strong = !inference.candidates[k].empty();
      for (const std::vector<const Type *> *list :
           {&inference.candidates[k], &inference.weakCandidates[k],
            &inference.literalCandidates[k]}) {
        for (const Type *c : *list) {
          if (type && list == &inference.literalCandidates[k] && type != c &&
              (type->isNumeric() || type->isDecimal())) {
            continue;
          }
          ResultCandidateJoin joined =
              TypeArgInferrer::candidatesJoin(generic, typeParams[k], type, c);
          if (joined.isErr() && strong && list == &inference.weakCandidates[k]) {
            continue;
          }
          if (joined.isErr()) {
            this->state.getDiagnostics().error(loc, joined.error().message);
            bound[k] = Type::error();
            inferred = false;
            return;
          }
          type = joined.value();
        }
      }
      const Type *constraint = typeParams[k]->constraint();
      if (constraint && constraint->isUnion()) {
        constraint = LiteralTypes::numberMemberOf(constraint, false);
      }
      if (type && type->isInteger() && inference.candidates[k].empty() &&
          inference.weakCandidates[k].empty() && constraint &&
          (constraint->isFloat() || constraint->isDecimal())) {
        type = constraint;
      }
      if (type && inference.undefinedSeen[k]) {
        type = Type::optional(type);
      } else if (!type && inference.undefinedSeen[k]) {
        type = Type::voidTy();
      }
      bound[k] = type;
    };

    std::vector<size_t> sensitive;
    auto makesResults = [&](const Expr *arg) {
      auto *name = dyn_cast<NameExpr>(arg);
      llvm::StringRef which =
          name ? ResultTypes(this->state).resultName(name->name) : llvm::StringRef();

      return which == "inferred" || which == "err";
    };
    for (size_t i = 0; i < call->args.size(); ++i) {
      Expr *&arg = call->args[i];
      const Type *pattern = paramOf(i);
      if (listRest && i >= fixed) {
        arg = ExprChecker(this->state).checkExpr(arg);
        checked[i] = true;
        continue;
      }
      if (!GenericCheckHelpers::mentionsAnyOf(pattern, typeParams)) {
        arg = ExprChecker(this->state).checkExpr(arg, pattern);
        checked[i] = true;
      } else if (auto *text = dyn_cast<StringLitExpr>(arg);
                 text && llvm::is_contained(typeParams, pattern) &&
                 UtilityTypes(this->state).wantsLiteral(pattern)) {
        const Type *literal = Type::stringLiteral(text->value);
        arg = ExprChecker(this->state).checkExpr(arg, literal);
        checked[i] = true;
        inference.from(pattern, literal);
      } else if (TypeArgInferrer::isContextSensitive(arg) || makesResults(arg) ||
                 (isa<NameExpr>(arg) &&
                  GenericChecker(this->state).genericNamed(cast<NameExpr>(arg)) &&
                  cast<NameExpr>(arg)->typeArgs.empty())) {
        sensitive.push_back(i);
      } else if (const Type *shape = GenericChecker::literalShape(arg)) {
        literals.push_back(i);
        inference.weak = true;
        inference.intLiteral = shape->isInteger() && TypeRelations::isIntLiteralTree(arg);
        inference.from(pattern, shape);
        inference.weak = false;
        inference.intLiteral = false;
      } else if (isa<ArrayLitExpr>(arg) || isa<ObjectLitExpr>(arg)) {
        sensitive.push_back(i);
        if (auto *lit = dyn_cast<ObjectLitExpr>(arg)) {
          this->precheckFields(lit, pattern, typeParams, inference);
        }
      } else {
        arg = ExprChecker(this->state).checkExpr(arg);
        checked[i] = true;
        inference.intLiteral = arg->type->isInteger() && TypeRelations::isIntLiteralTree(arg);
        inference.from(pattern, arg->type);
        inference.intLiteral = false;
      }
    }
    if (listRest) {
      std::vector<const Type *> parts;
      std::vector<bool> spread;
      for (size_t i = fixed; i < call->args.size(); ++i) {
        parts.push_back(call->args[i]->type);
        spread.push_back(call->spreads(i));
      }
      std::string why;
      if (const Type *list = ArgumentLists::spreadTuple(parts, spread, why)) {
        inference.from(listRest, list);
      }
    }
    for (size_t k = 0; k < typeParams.size(); ++k) {
      if (!inference.candidates[k].empty()) {
        settle(k);
      }
    }
    if (!inference.deferred.empty()) {
      inference.retry(bound);
      for (size_t k = 0; k < typeParams.size(); ++k) {
        if (!inference.candidates[k].empty()) {
          settle(k);
        }
      }
    }
    if (expected && llvm::is_contained(bound, nullptr) && ret &&
        GenericCheckHelpers::mentionsAnyOf(ret, typeParams)) {
      Inference fromExpected(typeParams, this->state.getAliasStore(), nullptr);
      fromExpected.from(ret, expected);
      for (size_t k = 0; k < typeParams.size(); ++k) {
        if (bound[k] || fromExpected.candidates[k].empty()) {
          continue;
        }
        const Type *type = nullptr;
        for (const Type *c : fromExpected.candidates[k]) {
          type = type ? TypeRelations::unify(type, c) : c;
        }
        bound[k] = type;
      }
    }
    for (size_t k = 0; k < typeParams.size(); ++k) {
      settle(k);
    }
    for (size_t i : sensitive) {
      Expr *&arg = call->args[i];
      const Type *pattern = paramOf(i);
      const Type *partial = TypeParams(this->state).substitute(pattern, typeParams, bound);
      const Type *fn = partial->nonOptional();
      if (fn->isFunction()) {
        Signature sig;
        for (const Type *p : fn->params()) {
          sig.params.push_back(GenericCheckHelpers::mentionsAnyOf(p, typeParams) ? Type::error()
                                                                                 : p);
        }
        sig.minArgs = fn->minArgs();
        sig.ret = GenericCheckHelpers::mentionsAnyOf(fn->ret(), typeParams) ? nullptr : fn->ret();
        sig.voidResult = fn->ret()->isVoid();
        auto *name = dyn_cast<NameExpr>(arg);
        auto *fe = dyn_cast<FunctionExpr>(arg);
        for (size_t p = 0;
             p < fn->params().size() && !(name && GenericChecker(this->state).genericNamed(name));
             ++p) {
          if (fe && p < fe->fn->params.size() && fe->fn->params[p].type) {
            continue;
          }
          for (size_t k = 0; k < typeParams.size(); ++k) {
            bool mentioned = GenericCheckHelpers::mentionsAnyOf(fn->params()[p], typeParams[k]);
            ResultCallbackParam known =
                TypeArgInferrer::callbackParam(generic, typeParams[k], bound[k], mentioned, fe, p);
            if (known.isErr()) {
              this->state.getDiagnostics().error(arg->loc(), known.error().message);
              bound[k] = Type::error();
              inferred = false;
            }
          }
        }
        if (fe) {
          arg = ClosureChecker(this->state).checkFunctionExpr(fe, &sig);
        } else if (FuncSymbol *other =
                       name ? GenericChecker(this->state).genericNamed(name) : nullptr) {
          arg = GenericInstantiator(this->state).checkGenericValue(name, other, &sig);
        } else if (FunctionExpr *wrapper =
                       FunctionValueChecker(this->state)
                           .builtinFunctionValue(arg, sig, unsigned(sig.params.size()), {})) {
          arg = ClosureChecker(this->state).checkFunctionExpr(wrapper, &sig);
        } else {
          arg = ExprChecker(this->state).checkExpr(arg);
        }
      } else {
        bool defaulted = false;
        for (size_t k = 0;
             k < typeParams.size() && GenericCheckHelpers::mentionsAnyOf(partial, typeParams);
             ++k) {
          if (!bound[k] && k < generic->typeDefaults.size() && generic->typeDefaults[k] &&
              !GenericCheckHelpers::mentionsAnyOf(generic->typeDefaults[k], typeParams)) {
            bound[k] = generic->typeDefaults[k];
            defaulted = true;
          }
        }
        if (defaulted) {
          partial = TypeParams(this->state).substitute(pattern, typeParams, bound);
        }
        arg =
            ExprChecker(this->state)
                .checkExpr(arg, GenericCheckHelpers::mentionsAnyOf(partial, typeParams) ? nullptr
                                                                                        : partial);
      }
      checked[i] = true;
      Inference more(typeParams, this->state.getAliasStore(), [this](const Type *t) {
        return IterableChecker(this->state).elementTypeOf(t);
      });
      more.from(pattern, arg->type);
      for (size_t k = 0; k < typeParams.size(); ++k) {
        if (!bound[k]) {
          inference.candidates[k] = more.candidates[k];
          inference.weakCandidates[k].clear();
          inference.literalCandidates[k].clear();
          inference.undefinedSeen[k] = more.undefinedSeen[k];
          settle(k);
        }
      }
    }
  }

  bool argErrors = false;
  for (size_t i = 0; i < call->args.size(); ++i) {
    argErrors |= checked[i] && call->args[i]->type->isError();
  }
  argErrors &= this->state.getDiagnostics().errorsSeen() > errorsBefore;
  inferred = this->finishTypeArgs(generic, bound, loc, true, argErrors) && inferred;
  for (size_t k = 0; k < typeParams.size() && inferred; ++k) {
    ResultArgNotGrowing growing = TypeArgInferrer::notGrowing(generic, bound[k], typeParams[k]);
    if (growing.isErr()) {
      this->state.getDiagnostics().error(loc, growing.error().message);
      inferred = false;
    }
  }
  if (!inferred) {
    for (size_t i = 0; i < call->args.size(); ++i) {
      if (!checked[i]) {
        call->args[i] = ExprChecker(this->state).checkExpr(call->args[i]);
      }
    }
    return false;
  }

  this->state.getGenericStore().enterInstanceLoc(loc);
  std::vector<const Type *> params;
  for (const Type *p : generic->params) {
    params.push_back(TypeParams(this->state).substituteValue(p, typeParams, bound));
  }
  for (size_t i : literals) {
    call->args[i] =
        ExprChecker(this->state)
            .checkExpr(call->args[i],
                       TypeParams(this->state).substituteValue(paramOf(i), typeParams, bound));
    checked[i] = true;
  }
  call->type = ret ? TypeParams(this->state).substitute(ret, typeParams, bound) : Type::error();
  this->state.getGenericStore().leaveInstanceLoc();
  size_t minArgs = generic->minArgs;
  bool hasRest = generic->hasRest;
  std::vector<const Type *> flat;
  if (ArgumentLists::flattenArguments(params, minArgs, hasRest, flat)) {
    params = std::move(flat);
  }
  ArgumentChecker(this->state)
      .checkArguments(call, params, minArgs, hasRest, loc, typeArgs.empty() ? &checked : nullptr);

  return ret != nullptr;
}

bool TypeArgInferrer::inferFromArguments(FuncSymbol *generic, llvm::ArrayRef<Expr *> args,
                                         std::vector<const Type *> &bound) {
  ReturnInferrer(this->state).resolveParams(generic);
  llvm::ArrayRef<const Type *> typeParams = generic->typeParams;
  Inference inference(typeParams, this->state.getAliasStore(), [this](const Type *t) {
    return IterableChecker(this->state).elementTypeOf(t);
  });
  inference.asyncOf = [this](const Type *t) {
    return IterableChecker(this->state).asyncIterableType(t);
  };
  size_t fixed = generic->params.size() - (generic->hasRest ? 1 : 0);
  for (size_t i = 0; i < args.size(); ++i) {
    const Type *pattern = nullptr;
    if (i < fixed) {
      pattern = generic->params[i];
    } else if (generic->hasRest && generic->params.back()->isArray()) {
      pattern = generic->params.back()->inner();
    }
    if (pattern && args[i]->type) {
      inference.from(pattern, args[i]->type);
    }
  }
  bound.assign(typeParams.size(), nullptr);
  for (size_t k = 0; k < typeParams.size(); ++k) {
    const Type *type = nullptr;
    for (const Type *c : inference.candidates[k]) {
      type = type ? TypeRelations::unify(type, c) : c;
      if (!type) {
        return false;
      }
    }
    if (type && inference.undefinedSeen[k]) {
      type = Type::optional(type);
    } else if (!type && inference.undefinedSeen[k]) {
      type = Type::voidTy();
    }
    bound[k] = type;
  }
  for (size_t k = 0; k < typeParams.size(); ++k) {
    if (!bound[k] && k < generic->typeDefaults.size() && generic->typeDefaults[k]) {
      bound[k] = TypeParams(this->state).substitute(generic->typeDefaults[k], typeParams, bound);
    }
    if (!bound[k] || GenericCheckHelpers::mentionsAnyOf(bound[k], typeParams) ||
        !TypeParams(this->state).satisfies(bound[k], typeParams[k], typeParams, bound)) {
      return false;
    }
  }

  return true;
}

}
