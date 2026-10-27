#include "compiler/check/functions/function_checker/function_checker.h"
#include "compiler/check/async/async_checker/async_checker.h"
#include "compiler/check/classes/class_checker/class_checker.h"
#include "compiler/check/expr/expr_checker/expr_checker.h"
#include "compiler/check/flow/flow_checker/flow_checker.h"
#include "compiler/check/flow/index_proofs/index_proofs.h"
#include "compiler/check/flow/predicate_checker/predicate_checker.h"
#include "compiler/check/functions/closure_checker/closure_checker.h"
#include "compiler/check/generics/arg_list_checker/arg_list_checker.h"
#include "compiler/check/generics/generic_class_checker/generic_class_checker.h"
#include "compiler/check/generics/type_params/type_params.h"
#include "compiler/check/iterators/generator_checker/generator_checker.h"
#include "compiler/check/iterators/iterable_checker/iterable_checker.h"
#include "compiler/check/scopes/scopes.h"
#include "compiler/check/shared_helpers/shared_helpers.h"
#include "compiler/check/stmt/return_checker/return_checker.h"
#include "compiler/check/types/type_relations/type_relations.h"
#include "compiler/check/types/type_resolver/type_resolver.h"
#include "compiler/declare/function_names/function_names.h"
#include "compiler/infer/return_inferrer/return_inferrer.h"
#include "compiler/instances/late_classes/late_classes.h"
#include "compiler/library/bytes/byte_proofs/byte_proofs.h"
#include "compiler/library/events/emitter_checker/emitter_checker.h"
#include "compiler/library/stream/chunk_types/chunk_types.h"
#include "compiler/library/tz_modules/tz_modules_checker/tz_modules_checker.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/ADT/StringSet.h"
#include "llvm/Support/ErrorHandling.h"
#include <cmath>
#include <functional>

using llvm::cast;
using llvm::dyn_cast;
using llvm::dyn_cast_or_null;
using llvm::isa;

namespace tz {

FunctionChecker::FunctionChecker(Checker &checker) : checker_(checker) {}

void FunctionChecker::declareFunction(FunctionDecl *fn, VarSymbol *var, bool scoped) {
  FuncSymbol *sym = checker_.ctx_.newFunc();
  sym->name = fn->name;
  sym->loc = fn->loc();
  sym->decl = fn;
  bool generic = !fn->typeParams.empty() || llvm::any_of(fn->params, [&](const Param &p) {
    return p.type && checker_.iterableChecker().isIterableRef(*p.type);
  });
  bool classParams = fn->cls && !fn->isStatic && !fn->cls->typeParams.empty();
  if (generic || classParams) {
    checker_.typeScopes_.emplace_back();
    if (classParams) {
      checker_.genericClassChecker().bindClassTypeParams(fn->cls);
    }
  }
  if (generic) {
    if (var) {
      if (!fn->typeParams.empty()) {
        checker_.diags_.error(fn->loc(),
                              "a generic function can be declared only at the top level for now");
      }
      fn->typeArgs.assign(fn->typeParams.size(), Type::error());
      checker_.typeParams().bindTypeParams(fn->typeParams, fn->typeArgs);
    } else {
      fn->typeArgs = sym->typeParams =
          checker_.typeParams().declareTypeParams(fn->typeParams, &sym->typeDefaults);
    }
  }
  std::optional<std::vector<const Type *>> hook =
      fn->cls ? checker_.chunkTypes().streamHookParams(fn) : std::nullopt;
  if (hook && llvm::none_of(fn->params, [](const Param &p) { return p.rest; })) {
    for (size_t i = fn->params.size(); i < hook->size(); ++i) {
      Param hidden;
      hidden.name = checker_.ctx_.hiddenName();
      hidden.loc = fn->loc();
      fn->params.push_back(hidden);
    }
  }
  bool afterOptional = false;
  for (size_t i = 0; i < fn->params.size(); ++i) {
    Param &param = fn->params[i];
    if (!param.decorators.empty() && (!fn->cls || fn->cls->literal) && !fn->isInstance) {
      checker_.diags_.error(param.decorators[0].loc,
                            "a decorator of a parameter can decorate only a "
                            "parameter of a method or a constructor of a class");
    }
    const Type *type = nullptr;
    if (param.type) {
      type = checker_.iterableChecker().iterableParam(fn, sym, *param.type);
      bool outerList = std::exchange(checker_.listContext_, param.rest);
      if (!type) {
        type = checker_.typeResolver().resolveType(*param.type, false);
      }
      checker_.listContext_ = outerList;
      if (hook && i < hook->size() && type && type->isFloat() && (*hook)[i]->isInt()) {
        type = (*hook)[i];
      }
    } else if (hook && i < hook->size() && !param.rest) {
      type = (*hook)[i];
    } else if (param.defaultValue && (type = literalType(param.defaultValue))) {
    } else if (param.defaultValue && !param.rest && !fn->source.empty()) {
      param.declaredType = nullptr;
      sym->params.push_back(nullptr);
      sym->pendingParams = true;
      continue;
    } else {
      checker_.diags_.error(param.loc, "parameter '" + param.label() + "' needs a type annotation");
      type = Type::error();
    }
    if (param.optional) {
      type = Type::optional(type);
    }
    param.declaredType = type;
    if (i == 0 && param.name == "this" && !param.rest && param.type) {
      if (fn->isArrow) {
        checker_.diags_.error(param.loc,
                              "an arrow function cannot have a 'this' parameter: it sees the "
                              "'this' of the function around it; write a 'function'");
      } else if (fn->cls) {
        checker_.diags_.error(param.loc,
                              "a method cannot have a 'this' parameter: its 'this' is its object");
      } else {
        sym->hasThis = !param.optional && !param.defaultValue;
      }
    }
    if (param.rest) {
      if (!type->isArray() && !type->isListParam() && !type->isTuple() && !type->isError()) {
        checker_.diags_.error(param.loc, "a rest parameter must be an array, e.g. '..." +
                                             param.name + ": int[]'");
        param.declaredType = Type::error();
      }
      sym->hasRest = true;
      sym->params.push_back(param.declaredType);
      continue;
    }
    bool mayOmit = param.optional || param.defaultValue;
    sym->params.push_back(mayOmit ? Type::optional(type) : type);
    if (!mayOmit) {
      if (afterOptional) {
        checker_.diags_.error(param.loc, "a required parameter cannot follow an optional one");
      }
      sym->minArgs = i + 1;
    }
    afterOptional |= param.optional;
  }
  if (fn->returnType) {
    sym->ret = checker_.asyncChecker().asyncReturnType(
        fn, checker_.generatorChecker().generatorReturnType(
                fn, checker_.predicateChecker().resolveReturnType(fn, sym)));
  } else if (!ReturnInferrer::returnsValue(fn->body) && fn->isAsync && !fn->isGenerator) {
    sym->ret = Type::promise(Type::voidTy());
  } else if (!ReturnInferrer::returnsValue(fn->body) && !fn->isGenerator) {
    sym->ret = Type::voidTy();
  }
  if (generic || classParams) {
    checker_.typeScopes_.pop_back();
  }
  checker_.argListChecker().expandArgumentList(fn, sym);
  fn->sym = sym;
  if (!fn->name.empty() && scoped) {
    checker_.scopes().declare(fn->name, fn->loc(), {var, sym});
  }
}

void FunctionChecker::checkFunctionBody(FunctionDecl *fn) {
  if (fn->external) {
    checker_.tzModulesChecker().checkExternal(fn);
    return;
  }
  FuncSymbol *sym = fn->sym;
  checker_.returnInferrer().resolveParams(sym);
  fn->checked = true;
  FuncSymbol *outer = checker_.currentFunction_;
  checker_.currentFunction_ = sym;
  checker_.functions_.push_back(fn);
  unsigned errors = checker_.diags_.errorCount();
  ClassDecl *outerClass = checker_.currentClass_;
  if (!fn->isClosure()) {
    checker_.currentClass_ = fn->cls;
  }
  FunctionDecl *outerInit = checker_.initFn_;
  const VarSymbol *outerThis = checker_.initThis_;
  bool outerReady = checker_.closureThisReady_;
  checker_.pushScope();
  if (fn->cls && !fn->isStatic) {
    checker_.genericClassChecker().bindClassTypeParams(fn->cls);
  }
  if (fn->cls) {
    checker_.lateClasses().bindOuterTypeParams(fn->cls);
  }
  if (!fn->typeArgs.empty()) {
    checker_.typeParams().bindTypeParams(fn->typeParams, fn->typeArgs);
  }
  if (fn->takesThis()) {
    VarSymbol *self = checker_.classChecker().declareThis(fn);
    if (fn->isConstructor || fn == fn->cls->init) {
      checker_.initFn_ = fn;
      checker_.initThis_ = self;
      checker_.closureThisReady_ = true;
      checker_.flow_.setFields.clear();
      bool derived = fn->cls->parent != nullptr;
      for (const ClassField &f : fn->cls->fields) {
        if (!f.isStatic && (f.optional || (fn->isConstructor && !derived && f.init && !f.method) ||
                            (!fn->isConstructor && f.redeclared))) {
          checker_.flow_.setFields.push_back(f.name);
        }
      }
      checker_.flow_.beforeSuper = derived && fn->isConstructor;
      if (checker_.flow_.beforeSuper) {
        unsigned supers = 0;
        for (Stmt *stmt : fn->body->body) {
          auto *expr = dyn_cast<ExprStmt>(stmt);
          auto *call = expr ? dyn_cast<CallExpr>(expr->expr) : nullptr;
          auto *callee = call ? dyn_cast<NameExpr>(call->callee) : nullptr;
          if (callee && callee->name == "super") {
            checker_.rootSupers_.insert(call);
            ++supers;
          }
        }
        if (supers == 0) {
          checker_.diags_.error(fn->loc(), "the constructor of '" + fn->cls->name +
                                               "' must call 'super(...)': '" + fn->cls->name +
                                               "' extends '" + fn->cls->parent->name +
                                               "', whose constructor sets the fields it declares");
        }
      }
    }
  }
  for (size_t i = 0; i < fn->params.size(); ++i) {
    Param &param = fn->params[i];
    const Type *type = param.declaredType;
    if (param.defaultValue && type->isError() && !param.type) {
      param.defaultValue->type = type;
      param.sym = checker_.scopes().declareVar(param.name, param.loc, type, false);
      continue;
    }
    if (param.defaultValue) {
      llvm::StringSet<> names;
      collectNames(param.defaultValue, names);
      bool forward = false;
      for (size_t j = i; j < fn->params.size() && !forward; ++j) {
        if (names.contains(fn->params[j].name)) {
          checker_.diags_.error(param.defaultValue->loc(),
                                "the default value of '" + param.label() + "' uses '" +
                                    fn->params[j].name +
                                    "', which is not set yet: a default value can "
                                    "use only the parameters before it");
          forward = true;
        }
      }
      if (forward) {
        param.defaultValue->type = Type::error();
        param.sym = checker_.scopes().declareVar(param.name, param.loc, type, false);
        continue;
      }
      if (auto *fe = dyn_cast<FunctionExpr>(param.defaultValue); fe && fe->fn->name.empty()) {
        fe->fn->name = param.name;
      }
      param.defaultValue =
          checker_.exprChecker().checkExpr(param.defaultValue, type->isError() ? nullptr : type);
      param.defaultValue = checker_.typeRelations().coerce(param.defaultValue, type);
    }
    param.sym = checker_.scopes().declareVar(param.name, param.loc, type, false);
    if (param.defaultValue && type->isOptional() && !param.defaultValue->type->isOptional() &&
        !param.defaultValue->type->isUndefined()) {
      checker_.flowChecker().setDefined(param.sym, true);
    }
  }
  checker_.byteProofs().addByteCallbackFacts(fn);
  checker_.indexProofs().addIndexCallbackFacts(fn);
  checker_.argListChecker().declareArgumentList(fn);
  checker_.predicateChecker().beginPredicate(fn);
  auto outerListened = std::move(checker_.flow_.errorListened);
  auto outerPending = std::move(checker_.flow_.errorPending);
  checker_.flow_.errorListened.clear();
  checker_.flow_.errorPending.clear();
  checker_.closureChecker().checkStatements(fn->body);
  if (!checker_.flow_.unreachable) {
    checker_.emitterChecker().reportUnlistenedErrors();
  }
  checker_.flow_.errorListened = std::move(outerListened);
  checker_.flow_.errorPending = std::move(outerPending);
  if (!checker_.flow_.unreachable && sym->asserts) {
    checker_.predicateChecker().notePredicateExit(checker_.flow_, Checker::Flow(), fn->body->loc());
  }
  checker_.predicateChecker().endPredicate(fn);
  if (fn->isConstructor && checker_.initFn_ == fn && !checker_.flow_.beforeSuper) {
    checker_.classChecker().checkFieldsSet(fn->loc());
  }
  checker_.popScope();
  checker_.functions_.pop_back();
  checker_.currentFunction_ = outer;
  checker_.currentClass_ = outerClass;
  checker_.initFn_ = outerInit;
  checker_.initThis_ = outerThis;
  checker_.closureThisReady_ = outerReady;

  const Type *ret = fn->isAsync && sym->ret && sym->ret->isPromise() ? sym->ret->promised()
                    : !fn->isGenerator                               ? sym->ret
                    : sym->ret && (sym->ret->isIterator() || sym->ret->isAsyncIterator())
                        ? sym->ret->returned()
                        : nullptr;
  if (ret && !ret->isVoid() && !ret->isError() && !ret->isOptional() && !ret->isUnknown() &&
      !ReturnChecker::alwaysReturns(fn->body)) {
    checker_.diags_.error(fn->loc(), functionLabel(fn->name) + " must return a value of type '" +
                                         ret->name() + "' on every path");
  }
  if (sym->isGeneric() && checker_.diags_.errorCount() > errors) {
    checker_.brokenGenerics_.insert(sym);
  }
  if (fn->cls && fn->cls->isGeneric() && checker_.diags_.errorCount() > errors) {
    checker_.brokenClasses_.insert(fn->cls);
  }
}

}
