#include "compiler/check/generics/type_arg_inferrer/type_arg_inferrer.h"
#include "compiler/check/expr/argument_checker/argument_checker.h"
#include "compiler/check/expr/call_checker/call_checker.h"
#include "compiler/check/expr/expr_checker/expr_checker.h"
#include "compiler/check/functions/closure_checker/closure_checker.h"
#include "compiler/check/functions/function_value_checker/function_value_checker.h"
#include "compiler/check/generics/generic_check_helpers/generic_check_helpers.h"
#include "compiler/check/generics/generic_checker/generic_checker.h"
#include "compiler/check/generics/generic_instantiator/generic_instantiator.h"
#include "compiler/check/generics/inference/inference.h"
#include "compiler/check/generics/type_params/type_params.h"
#include "compiler/check/iterators/iterable_checker/iterable_checker.h"
#include "compiler/check/types/type_relations/type_relations.h"
#include "compiler/check/types/type_resolver/type_resolver.h"
#include "compiler/check/types/utility_types/utility_types.h"
#include "compiler/infer/return_inferrer/return_inferrer.h"
#include "compiler/instances/late_classes/late_classes.h"
#include "compiler/library/result/result_types/result_types.h"
#include "llvm/ADT/STLExtras.h"
#include <functional>
#include <set>

using llvm::cast;
using llvm::dyn_cast;
using llvm::dyn_cast_or_null;
using llvm::isa;

namespace tz {

namespace {

bool grows(const Type *type, llvm::ArrayRef<const Type *> params, bool inside) {
  switch (type->kind()) {
  case Type::Kind::Param:
    return inside && llvm::is_contained(params, type);
  case Type::Kind::Optional:
    return grows(type->inner(), params, inside);
  case Type::Kind::Array:
  case Type::Kind::Promise:
    return grows(type->inner(), params, true);
  case Type::Kind::Result:
    return grows(type->okType(), params, true) || grows(type->errType(), params, true);
  case Type::Kind::Function:
    return grows(type->ret(), params, true) ||
           llvm::any_of(type->params(), [&](const Type *p) { return grows(p, params, true); });
  case Type::Kind::Object:
  case Type::Kind::Iterator:
  case Type::Kind::Dict:
    return mentionsAnyOf(type, params);
  default:
    return false;
  }
}

bool isContextSensitive(const Expr *arg) {
  if (auto *fe = dyn_cast<FunctionExpr>(arg)) {
    return llvm::any_of(fe->fn->params, [](const Param &p) { return !p.type; });
  }
  if (auto *lit = dyn_cast<ArrayLitExpr>(arg)) {
    return llvm::any_of(lit->elements,
                        [](const ArrayLitExpr::Element &e) { return isContextSensitive(e.expr); });
  }
  if (auto *lit = dyn_cast<ObjectLitExpr>(arg)) {
    return llvm::any_of(
        lit->props, [](const ObjectLitExpr::Property &p) { return isContextSensitive(p.value); });
  }
  if (auto *name = dyn_cast<NameExpr>(arg)) {
    return name->typeArgs.empty() &&
           llvm::is_contained(
               llvm::ArrayRef<llvm::StringRef>{"String", "Number", "parseInt", "parseFloat",
                                               "isNaN", "isFinite", "Boolean", "encodeURIComponent",
                                               "encodeURI", "decodeURIComponent", "decodeURI"},
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

}

TypeArgInferrer::TypeArgInferrer(Checker &checker) : checker_(checker) {}

bool TypeArgInferrer::finishTypeArgs(FuncSymbol *generic, std::vector<const Type *> &bound,
                                     llvm::SMLoc loc, bool call, bool quiet) {
  llvm::ArrayRef<const Type *> params = generic->typeParams;
  bool ok = true;
  for (size_t k = 0; k < params.size(); ++k) {
    if (!bound[k] && k < generic->typeDefaults.size() && generic->typeDefaults[k]) {
      const Type *fallback =
          checker_.typeParams().substitute(generic->typeDefaults[k], params, bound);
      bool open = false;
      for (size_t j = 0; j < params.size(); ++j) {
        open |= !bound[j] && mentionsAnyOf(fallback, params[j]);
      }
      if (!open) {
        bound[k] = fallback;
      }
    }
    if (bound[k] && bound[k]->isError()) {
      ok = false;
      continue;
    }
    if (!bound[k] && quiet) {
      bound[k] = Type::error();
      ok = false;
      continue;
    }
    if (!bound[k]) {
      std::string ints;
      for (size_t i = 0; i < params.size(); ++i) {
        ints += (i == 0 ? "" : ", ") + std::string(params[i]->isListParam() ? "[int]" : "int");
      }
      checker_.diags_.error(loc, "cannot infer type parameter '" + params[k]->name() + "' of '" +
                                     generic->name + "': nothing " +
                                     (call ? "in the call" : "here") +
                                     " says what it is; give it, e.g. '" + generic->name + "<" +
                                     ints + ">" + (call ? "(...)" : "") + "'");
      bound[k] = Type::error();
      ok = false;
      continue;
    }
    if (const Type *element = params[k]->iterated()) {
      const Type *wanted = checker_.typeParams().substitute(element, params, bound);
      const Type *given = checker_.iterableChecker().elementTypeOf(bound[k]);
      if (given == wanted || wanted->isError() || bound[k]->isError()) {
        continue;
      }
      checker_.diags_.error(
          loc, given ? "'" + bound[k]->name() + "' gives values of type '" + given->name() +
                           "', and the parameter 'Iterable<" + wanted->name() + ">' of '" +
                           generic->name + "' takes values of type '" + wanted->name() + "'"
                     : "'" + bound[k]->name() +
                           "' cannot be iterated, and the parameter "
                           "'Iterable<" +
                           wanted->name() + ">' of '" + generic->name +
                           "' takes something that can: an array, a string, a Map, a Set, an "
                           "iterator, or an object with '[Symbol.iterator]()'");
      bound[k] = Type::error();
      ok = false;
      continue;
    }
    if (!checker_.typeParams().satisfies(bound[k], params[k], params, bound)) {
      llvm::ArrayRef<TypeParamDecl> decls = generic->decl->typeParams;
      checker_.diags_.error(
          loc, "type '" + bound[k]->name() + "' does not satisfy the constraint '" +
                   constraintSpelling(params[k], k < decls.size() ? &decls[k] : nullptr) +
                   "' of '" + params[k]->name() + "' in '" + generic->name + "'");
      bound[k] = Type::error();
      ok = false;
    }
  }

  return ok;
}

Expr *TypeArgInferrer::checkGenericCall(CallExpr *call, FuncSymbol *generic, llvm::SMLoc loc,
                                        const Type *expected, llvm::ArrayRef<TypeRef> typeArgs) {
  std::vector<const Type *> bound;
  if (!bindGenericCall(call, generic, loc, expected, typeArgs, bound)) {
    return call;
  }
  call->func = generic;
  const ClassDecl *cls = generic->decl ? generic->decl->cls : nullptr;
  bool concrete = llvm::none_of(bound, [](const Type *t) { return mentionsParam(t); }) &&
                  (!cls || generic->decl->isStatic || cls->isEmitted());
  bool copiesReturn = checker_.lateClasses().isTemplate(generic) && !generic->ret;
  if (concrete && !checker_.trial_ && !generic->inferring && (generic->ret || copiesReturn)) {
    if (FuncSymbol *instance = checker_.genericInstantiator().instantiate(
            generic, bound, loc, checker_.genericInstantiator().exactClassArgs(call, generic))) {
      call->func = instance;
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
  unsigned errorsBefore = checker_.diags_.errorsSeen();
  checker_.returnInferrer().resolveParams(generic);
  const Type *ret = checker_.returnInferrer().returnType(generic);
  if (checker_.genericInstantiator().returnsOwnClass(generic, ret)) {
    ret = nullptr;
  }
  llvm::ArrayRef<const Type *> typeParams = generic->typeParams;
  size_t fixed = generic->params.size() - (generic->hasRest ? 1 : 0);
  for (size_t i = 0; i < call->args.size() && i < (generic->hasRest ? fixed : call->args.size());
       ++i) {
    if (call->spreads(i) && checker_.callChecker().rejectSpread(call)) {
      return false;
    }
  }
  if (!checker_.argumentChecker().checkArity(
          call, loc, static_cast<unsigned>(generic->minArgs),
          generic->hasRest ? ~0u : static_cast<unsigned>(generic->params.size()))) {
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
                                              spreadsLists(generic->params.back()))
                             ? generic->params.back()
                             : nullptr;

  bound.assign(typeParams.size(), nullptr);
  std::vector<bool> checked(call->args.size(), false);
  std::vector<size_t> literals;
  bool ok = true;
  if (!typeArgs.empty()) {
    if (typeArgs.size() > typeParams.size()) {
      checker_.diags_.error(loc, "'" + generic->name + "' takes " +
                                     std::to_string(typeParams.size()) + " type argument" +
                                     (typeParams.size() == 1 ? "" : "s") + ", but got " +
                                     std::to_string(typeArgs.size()));
      ok = false;
    }
    for (size_t k = 0; k < typeArgs.size() && k < typeParams.size(); ++k) {
      bound[k] = checker_.typeResolver().resolveType(typeArgs[k], false);
    }
  } else {
    Inference inference(typeParams, checker_.aliasOrigins_, [this](const Type *t) {
      return checker_.iterableChecker().elementTypeOf(t);
    });
    inference.asyncOf = [this](const Type *t) {
      return checker_.iterableChecker().asyncIterableType(t);
    };
    auto settle = [&](size_t k) {
      if (bound[k]) {
        return;
      }
      const Type *type = nullptr;
      bool strong = !inference.candidates[k].empty();
      for (const std::vector<const Type *> *list :
           {&inference.candidates[k], &inference.weakCandidates[k]}) {
        for (const Type *c : *list) {
          const Type *next = type ? TypeRelations::unify(type, c) : c;
          if (!next && strong && list == &inference.weakCandidates[k]) {
            continue;
          }
          if (!next) {
            checker_.diags_.error(loc, "type parameter '" + typeParams[k]->name() + "' of '" +
                                           generic->name + "' gets '" + type->name() +
                                           "' from one argument and '" + c->name() +
                                           "' from another; give it, e.g. '" + generic->name + "<" +
                                           type->name() + ">(...)'");
            bound[k] = Type::error();
            ok = false;
            return;
          }
          type = next;
        }
      }
      if (type && inference.undefinedSeen[k]) {
        type = Type::optional(type);
      }
      bound[k] = type;
    };

    std::vector<size_t> sensitive;
    auto makesResults = [&](const Expr *arg) {
      auto *name = dyn_cast<NameExpr>(arg);
      llvm::StringRef which =
          name ? checker_.resultTypes().resultName(name->name) : llvm::StringRef();

      return which == "ok" || which == "err";
    };
    for (size_t i = 0; i < call->args.size(); ++i) {
      Expr *&arg = call->args[i];
      const Type *pattern = paramOf(i);
      if (listRest && i >= fixed) {
        arg = checker_.exprChecker().checkExpr(arg);
        checked[i] = true;
        continue;
      }
      if (!mentionsAnyOf(pattern, typeParams)) {
        arg = checker_.exprChecker().checkExpr(arg, pattern);
        checked[i] = true;
      } else if (auto *text = dyn_cast<StringLitExpr>(arg);
                 text && llvm::is_contained(typeParams, pattern) &&
                 checker_.utilityTypes().wantsLiteral(pattern)) {
        const Type *literal = Type::stringLiteral(text->value);
        arg = checker_.exprChecker().checkExpr(arg, literal);
        checked[i] = true;
        inference.from(pattern, literal);
      } else if (isContextSensitive(arg) || makesResults(arg) ||
                 (isa<NameExpr>(arg) &&
                  checker_.genericChecker().genericNamed(cast<NameExpr>(arg)) &&
                  cast<NameExpr>(arg)->typeArgs.empty())) {
        sensitive.push_back(i);
      } else if (const Type *shape = GenericChecker::literalShape(arg)) {
        literals.push_back(i);
        inference.weak = true;
        inference.from(pattern, shape);
        inference.weak = false;
      } else if (isa<ArrayLitExpr>(arg) || isa<ObjectLitExpr>(arg)) {
        sensitive.push_back(i);
      } else {
        arg = checker_.exprChecker().checkExpr(arg);
        checked[i] = true;
        inference.from(pattern, arg->type);
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
      if (const Type *list = spreadTuple(parts, spread, why)) {
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
    if (expected && llvm::is_contained(bound, nullptr) && ret && mentionsAnyOf(ret, typeParams)) {
      Inference fromExpected(typeParams, checker_.aliasOrigins_, nullptr);
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
      const Type *partial = checker_.typeParams().substitute(pattern, typeParams, bound);
      const Type *fn = partial->nonOptional();
      if (fn->isFunction()) {
        Checker::Signature sig;
        for (const Type *p : fn->params()) {
          sig.params.push_back(mentionsAnyOf(p, typeParams) ? Type::error() : p);
        }
        sig.minArgs = fn->minArgs();
        sig.ret = mentionsAnyOf(fn->ret(), typeParams) ? nullptr : fn->ret();
        sig.voidResult = fn->ret()->isVoid();
        auto *name = dyn_cast<NameExpr>(arg);
        auto *fe = dyn_cast<FunctionExpr>(arg);
        for (size_t p = 0;
             p < fn->params().size() && !(name && checker_.genericChecker().genericNamed(name));
             ++p) {
          if (fe && p < fe->fn->params.size() && fe->fn->params[p].type) {
            continue;
          }
          for (size_t k = 0; k < typeParams.size(); ++k) {
            if (bound[k] || !mentionsAnyOf(fn->params()[p], typeParams[k])) {
              continue;
            }
            std::string what =
                fe && p < fe->fn->params.size()
                    ? "parameter '" + fe->fn->params[p].name.str() + "' of this function"
                    : "the parameters of this function";
            checker_.diags_.error(
                arg->loc(), "cannot infer type parameter '" + typeParams[k]->name() + "' of '" +
                                generic->name + "' for " + what +
                                ": nothing else in the call says what it is; give it, e.g. '" +
                                generic->name + "<int>(...)'" +
                                (fe ? ", or the type of the parameter" : ""));
            bound[k] = Type::error();
            ok = false;
          }
        }
        if (fe) {
          arg = checker_.closureChecker().checkFunctionExpr(fe, &sig);
        } else if (FuncSymbol *other =
                       name ? checker_.genericChecker().genericNamed(name) : nullptr) {
          arg = checker_.genericInstantiator().checkGenericValue(name, other, &sig);
        } else if (FunctionExpr *wrapper = checker_.functionValueChecker().builtinFunctionValue(
                       arg, sig, unsigned(sig.params.size()), {})) {
          arg = checker_.closureChecker().checkFunctionExpr(wrapper, &sig);
        } else {
          arg = checker_.exprChecker().checkExpr(arg);
        }
      } else {
        bool defaulted = false;
        for (size_t k = 0; k < typeParams.size() && mentionsAnyOf(partial, typeParams); ++k) {
          if (!bound[k] && k < generic->typeDefaults.size() && generic->typeDefaults[k] &&
              !mentionsAnyOf(generic->typeDefaults[k], typeParams)) {
            bound[k] = generic->typeDefaults[k];
            defaulted = true;
          }
        }
        if (defaulted) {
          partial = checker_.typeParams().substitute(pattern, typeParams, bound);
        }
        arg = checker_.exprChecker().checkExpr(arg, mentionsAnyOf(partial, typeParams) ? nullptr
                                                                                       : partial);
      }
      checked[i] = true;
      Inference more(typeParams, checker_.aliasOrigins_,
                     [this](const Type *t) { return checker_.iterableChecker().elementTypeOf(t); });
      more.from(pattern, arg->type);
      for (size_t k = 0; k < typeParams.size(); ++k) {
        if (!bound[k]) {
          inference.candidates[k] = more.candidates[k];
          inference.weakCandidates[k].clear();
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
  argErrors &= checker_.diags_.errorsSeen() > errorsBefore;
  ok = finishTypeArgs(generic, bound, loc, true, argErrors) && ok;
  for (size_t k = 0; k < typeParams.size() && ok; ++k) {
    if (!grows(bound[k], typeParams, false)) {
      continue;
    }
    checker_.diags_.error(
        loc, "'" + generic->name + "' calls itself with '" + bound[k]->name() + "' for '" +
                 typeParams[k]->name() +
                 "', so each copy of it for some type needs another for a "
                 "bigger one, without end; a generic function can call itself only with its own "
                 "type parameters");
    ok = false;
  }
  if (!ok) {
    for (size_t i = 0; i < call->args.size(); ++i) {
      if (!checked[i]) {
        call->args[i] = checker_.exprChecker().checkExpr(call->args[i]);
      }
    }
    return false;
  }

  llvm::SMLoc outerLoc = std::exchange(checker_.instanceLoc_, loc);
  std::vector<const Type *> params;
  for (const Type *p : generic->params) {
    params.push_back(checker_.typeParams().substitute(p, typeParams, bound));
  }
  for (size_t i : literals) {
    call->args[i] = checker_.exprChecker().checkExpr(
        call->args[i], checker_.typeParams().substitute(paramOf(i), typeParams, bound));
    checked[i] = true;
  }
  call->type = ret ? checker_.typeParams().substitute(ret, typeParams, bound) : Type::error();
  checker_.instanceLoc_ = outerLoc;
  size_t minArgs = generic->minArgs;
  bool hasRest = generic->hasRest;
  std::vector<const Type *> flat;
  if (flattenArguments(params, minArgs, hasRest, flat)) {
    params = std::move(flat);
  }
  checker_.argumentChecker().checkArguments(call, params, minArgs, hasRest, loc,
                                            typeArgs.empty() ? &checked : nullptr);

  return ret != nullptr;
}

bool TypeArgInferrer::inferFromArguments(FuncSymbol *generic, llvm::ArrayRef<Expr *> args,
                                         std::vector<const Type *> &bound) {
  checker_.returnInferrer().resolveParams(generic);
  llvm::ArrayRef<const Type *> typeParams = generic->typeParams;
  Inference inference(typeParams, checker_.aliasOrigins_, [this](const Type *t) {
    return checker_.iterableChecker().elementTypeOf(t);
  });
  inference.asyncOf = [this](const Type *t) {
    return checker_.iterableChecker().asyncIterableType(t);
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
    }
    bound[k] = type;
  }
  for (size_t k = 0; k < typeParams.size(); ++k) {
    if (!bound[k] && k < generic->typeDefaults.size() && generic->typeDefaults[k]) {
      bound[k] = checker_.typeParams().substitute(generic->typeDefaults[k], typeParams, bound);
    }
    if (!bound[k] || mentionsAnyOf(bound[k], typeParams) ||
        !checker_.typeParams().satisfies(bound[k], typeParams[k], typeParams, bound)) {
      return false;
    }
  }

  return true;
}

}
