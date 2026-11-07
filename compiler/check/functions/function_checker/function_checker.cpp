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
#include <cmath>

using llvm::dyn_cast;

namespace tz {

FunctionChecker::FunctionChecker(Checker &owner)
    : checker(owner) {}

void FunctionChecker::declareFunction(FunctionDecl *fn, VarSymbol *var, bool scoped) {
  FuncSymbol *sym = this->checker.ctx_.newFunc();
  sym->name = fn->name;
  sym->loc = fn->loc();
  sym->decl = fn;
  bool generic = !fn->typeParams.empty() || llvm::any_of(fn->params, [&](const Param &p) {
    return p.type && this->checker.iterableChecker().isIterableRef(*p.type);
  });
  bool classParams = fn->cls() && !fn->isStatic() && !fn->cls()->typeParams.empty();
  if (generic || classParams) {
    this->checker.typeScopes_.emplace_back();
    if (classParams) {
      this->checker.genericClassChecker().bindClassTypeParams(fn->cls());
    }
  }
  if (generic) {
    if (var) {
      if (!fn->typeParams.empty()) {
        this->checker.diags_.error(
            fn->loc(), "a generic function can be declared only at the top level for now");
      }
      fn->typeArgs.assign(fn->typeParams.size(), Type::error());
      this->checker.typeParams().bindTypeParams(fn->typeParams, fn->typeArgs);
    } else {
      fn->typeArgs = sym->typeParams =
          this->checker.typeParams().declareTypeParams(fn->typeParams, &sym->typeDefaults);
    }
  }
  std::optional<std::vector<const Type *>> hook =
      fn->cls() ? this->checker.chunkTypes().streamHookParams(fn) : std::nullopt;
  if (hook && llvm::none_of(fn->params, [](const Param &p) { return p.rest(); })) {
    for (size_t i = fn->params.size(); i < hook->size(); ++i) {
      Param hidden;
      hidden.name = this->checker.ctx_.hiddenName();
      hidden.loc = fn->loc();
      fn->params.push_back(hidden);
    }
  }
  bool afterOptional = false;
  for (size_t i = 0; i < fn->params.size(); ++i) {
    Param &param = fn->params[i];
    if (!param.decorators.empty() && (!fn->cls() || fn->cls()->literal()) && !fn->isInstance) {
      this->checker.diags_.error(param.decorators[0].loc,
                                 "a decorator of a parameter can decorate only a "
                                 "parameter of a method or a constructor of a class");
    }
    const Type *type = nullptr;
    if (param.type) {
      IterableChecker::ResultIterableParam iterable =
          this->checker.iterableChecker().iterableParam(fn, sym, *param.type);
      if (iterable.isOk()) {
        type = iterable.value();
      } else if (iterable.error().code != IterableCheckerIterableParamErrorCode::NOT_ITERABLE) {
        this->checker.diags_.error(param.type->loc, iterable.error().message);
        type = Type::error();
      }
      bool outerList = std::exchange(this->checker.listContext_, param.rest());
      if (!type) {
        type = this->checker.typeResolver().resolveType(*param.type, false);
      }
      this->checker.listContext_ = outerList;
      if (hook && i < hook->size() && type && type->isFloat() && (*hook)[i]->isInt()) {
        type = (*hook)[i];
      }
    } else if (hook && i < hook->size() && !param.rest()) {
      type = (*hook)[i];
    } else if (param.defaultValue() && (type = literalType(param.defaultValue()))) {
    } else if (param.defaultValue() && !param.rest() && !fn->source.empty()) {
      param.declaredType = nullptr;
      sym->params.push_back(nullptr);
      sym->pendingParams = true;
      continue;
    } else {
      this->checker.diags_.error(param.loc,
                                 "parameter '" + param.label() + "' needs a type annotation");
      type = Type::error();
    }
    if (param.optional()) {
      type = Type::optional(type);
    }
    param.declaredType = type;
    if (i == 0 && param.name == "this" && !param.rest() && param.type) {
      if (fn->isArrow()) {
        this->checker.diags_.error(param.loc,
                                   "an arrow function cannot have a 'this' parameter: it sees the "
                                   "'this' of the function around it; write a 'function'");
      } else if (fn->cls()) {
        this->checker.diags_.error(
            param.loc, "a method cannot have a 'this' parameter: its 'this' is its object");
      } else {
        sym->hasThis = !param.optional() && !param.defaultValue();
      }
    }
    if (param.rest()) {
      if (!type->isArray() && !type->isListParam() && !type->isTuple() && !type->isError()) {
        this->checker.diags_.error(param.loc, "a rest parameter must be an array, e.g. '..." +
                                                  param.name + ": int[]'");
        param.declaredType = Type::error();
      }
      sym->hasRest = true;
      sym->params.push_back(param.declaredType);
      continue;
    }
    bool mayOmit = param.optional() || param.defaultValue();
    sym->params.push_back(mayOmit ? Type::optional(type) : type);
    if (!mayOmit) {
      if (afterOptional) {
        this->checker.diags_.error(param.loc, "a required parameter cannot follow an optional one");
      }
      sym->minArgs = i + 1;
    }
    afterOptional |= param.optional();
  }
  if (fn->returnType) {
    sym->ret = this->checker.asyncChecker().asyncReturnType(
        fn, this->checker.generatorChecker().generatorReturnType(
                fn, this->checker.predicateChecker().resolveReturnType(fn, sym)));
  } else if (!ReturnInferrer::returnsValue(fn->body) && fn->isAsync && !fn->isGenerator) {
    sym->ret = Type::promise(Type::voidTy());
  } else if (!ReturnInferrer::returnsValue(fn->body) && !fn->isGenerator) {
    sym->ret = Type::voidTy();
  }
  if (generic || classParams) {
    this->checker.typeScopes_.pop_back();
  }
  this->checker.argListChecker().expandArgumentList(fn, sym);
  fn->sym = sym;
  if (!fn->name.empty() && scoped) {
    Scopes::ResultEntryDeclared added = this->checker.scopes().declare(
        fn->name, fn->loc(), {FunctionEntry{.func = sym, .var = var}});
    if (added.isErr()) {
      this->checker.diags_.error(fn->loc(), added.error().message);
    }
    if (added.isErr() && added.error().code != ScopesDeclareErrorCode::UNDEFINED_AS_NAME) {
      ScopeEntry previousEntry = this->checker.scopes().lookup(fn->name);
      std::string declaredHere = ErrorTextDeclare::previous();
      this->checker.diags_.note(previousEntry.loc(), declaredHere);
    }
  }
}

void FunctionChecker::checkFunctionBody(FunctionDecl *fn) {
  if (fn->intrinsic()) {
    fn->checked = true;
    return;
  }
  if (fn->external()) {
    this->checker.tzModulesChecker().checkExternal(fn);
    return;
  }
  FuncSymbol *sym = fn->sym;
  this->checker.returnInferrer().resolveParams(sym);
  fn->checked = true;
  FuncSymbol *outer = this->checker.currentFunction_;
  this->checker.currentFunction_ = sym;
  this->checker.functions_.push_back(fn);
  unsigned errors = this->checker.diags_.errorCount();
  ClassDecl *outerClass = this->checker.currentClass_;
  if (!fn->isClosure()) {
    this->checker.currentClass_ = fn->cls();
  }
  FunctionDecl *outerInit = this->checker.initFn_;
  const VarSymbol *outerThis = this->checker.initThis_;
  bool outerReady = this->checker.closureThisReady_;
  this->checker.pushScope();
  if (fn->cls() && !fn->isStatic()) {
    this->checker.genericClassChecker().bindClassTypeParams(fn->cls());
  }
  if (fn->cls()) {
    this->checker.lateClasses().bindOuterTypeParams(fn->cls());
  }
  if (!fn->typeArgs.empty()) {
    this->checker.typeParams().bindTypeParams(fn->typeParams, fn->typeArgs);
  }
  if (fn->takesThis()) {
    ClassChecker::ResultNewThisSymbol declared = this->checker.classChecker().declareThis(fn);
    if (declared.isErr()) {
      this->checker.diags_.error(fn->loc(), declared.error().message);
    }
    VarSymbol *self = declared.isOk() ? declared.value() : nullptr;
    if (fn->isConstructor() || fn == fn->cls()->init) {
      this->checker.initFn_ = fn;
      this->checker.initThis_ = self;
      this->checker.closureThisReady_ = true;
      this->checker.flow_.setFields.clear();
      bool derived = fn->cls()->parent != nullptr;
      for (const ClassField &f : fn->cls()->fields) {
        if (!f.isStatic() &&
            (f.optional() || (fn->isConstructor() && !derived && f.init && !f.method()) ||
             (!fn->isConstructor() && f.redeclared))) {
          this->checker.flow_.setFields.push_back(f.name);
        }
      }
      this->checker.flow_.beforeSuper = derived && fn->isConstructor();
      if (this->checker.flow_.beforeSuper) {
        unsigned supers = 0;
        for (Stmt *stmt : fn->body->body) {
          auto *expr = dyn_cast<ExprStmt>(stmt);
          auto *call = expr ? dyn_cast<CallExpr>(expr->expr) : nullptr;
          auto *callee = call ? dyn_cast<NameExpr>(call->callee) : nullptr;
          if (callee && callee->name == "super") {
            this->checker.rootSupers_.insert(call);
            ++supers;
          }
        }
        if (supers == 0) {
          this->checker.diags_.error(
              fn->loc(), "the constructor of '" + fn->cls()->name + "' must call 'super(...)': '" +
                             fn->cls()->name + "' extends '" + fn->cls()->parent->name +
                             "', whose constructor sets the fields it declares");
        }
      }
    }
  }
  for (size_t i = 0; i < fn->params.size(); ++i) {
    Param &param = fn->params[i];
    const Type *type = param.declaredType;
    if (param.defaultValue() && type->isError() && !param.type) {
      param.defaultValue()->type = type;
      param.sym = this->checker.scopes().declareVar(param.name, param.loc, type, false);
      continue;
    }
    if (DefaultedParam *defaulted = param.form.as<DefaultedParam>()) {
      llvm::StringSet<> names;
      collectNames(defaulted->defaultValue, names);
      bool forward = false;
      for (size_t j = i; j < fn->params.size() && !forward; ++j) {
        if (names.contains(fn->params[j].name)) {
          this->checker.diags_.error(defaulted->defaultValue->loc(),
                                     "the default value of '" + param.label() + "' uses '" +
                                         fn->params[j].name +
                                         "', which is not set yet: a default value can "
                                         "use only the parameters before it");
          forward = true;
        }
      }
      if (forward) {
        defaulted->defaultValue->type = Type::error();
        param.sym = this->checker.scopes().declareVar(param.name, param.loc, type, false);
        continue;
      }
      if (auto *fe = dyn_cast<FunctionExpr>(defaulted->defaultValue); fe && fe->fn->name.empty()) {
        fe->fn->name = param.name;
      }
      Expr *checked = this->checker.exprChecker().checkExpr(defaulted->defaultValue,
                                                            type->isError() ? nullptr : type);
      defaulted->defaultValue = this->checker.typeRelations().coerce(checked, type);
    }
    param.sym = this->checker.scopes().declareVar(param.name, param.loc, type, false);
    if (param.defaultValue() && type->isOptional() && !param.defaultValue()->type->isOptional() &&
        !param.defaultValue()->type->isUndefined()) {
      this->checker.flowChecker().setDefined(param.sym, true);
    }
  }
  this->checker.byteProofs().addByteCallbackFacts(fn);
  this->checker.indexProofs().addIndexCallbackFacts(fn);
  this->checker.argListChecker().declareArgumentList(fn);
  this->checker.predicateChecker().beginPredicate(fn);
  auto outerListened = std::move(this->checker.flow_.errorListened);
  auto outerPending = std::move(this->checker.flow_.errorPending);
  this->checker.flow_.errorListened.clear();
  this->checker.flow_.errorPending.clear();
  this->checker.closureChecker().checkStatements(fn->body);
  if (!this->checker.flow_.unreachable) {
    this->checker.emitterChecker().reportUnlistenedErrors();
  }
  this->checker.flow_.errorListened = std::move(outerListened);
  this->checker.flow_.errorPending = std::move(outerPending);
  this->checker.predicateChecker().endPredicate(fn);
  if (fn->isConstructor() && this->checker.initFn_ == fn && !this->checker.flow_.beforeSuper) {
    ClassChecker::ResultEveryFieldSet fieldsSet = this->checker.classChecker().checkFieldsSet();
    if (fieldsSet.isErr()) {
      this->checker.diags_.error(fn->loc(), fieldsSet.error().message);
    }
  }
  this->checker.popScope();
  this->checker.functions_.pop_back();
  this->checker.currentFunction_ = outer;
  this->checker.currentClass_ = outerClass;
  this->checker.initFn_ = outerInit;
  this->checker.initThis_ = outerThis;
  this->checker.closureThisReady_ = outerReady;

  const Type *ret = fn->isAsync && sym->ret && sym->ret->isPromise() ? sym->ret->promised()
                    : !fn->isGenerator                               ? sym->ret
                    : sym->ret && (sym->ret->isIterator() || sym->ret->isAsyncIterator())
                        ? sym->ret->returned()
                        : nullptr;
  if (ret && !ret->isVoid() && !ret->isError() && !ret->isOptional() && !ret->isUnknown() &&
      !ReturnChecker::alwaysReturns(fn->body)) {
    this->checker.diags_.error(fn->loc(), functionLabel(fn->name) +
                                              " must return a value of type '" + ret->name() +
                                              "' on every path");
  }
  if (sym->isGeneric() && this->checker.diags_.errorCount() > errors) {
    this->checker.brokenGenerics_.insert(sym);
  }
  if (fn->cls() && fn->cls()->isGeneric() && this->checker.diags_.errorCount() > errors) {
    this->checker.brokenClasses_.insert(fn->cls());
  }
}

}
