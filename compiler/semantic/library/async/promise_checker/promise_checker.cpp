#include "compiler/semantic/library/async/promise_checker/promise_checker.h"
#include "compiler/program/ast_walker/ast_walker.h"
#include "compiler/semantic/bodies/expr/argument_checker/argument_checker.h"
#include "compiler/semantic/bodies/expr/call_checker/call_checker.h"
#include "compiler/semantic/bodies/expr/expr_checker/expr_checker.h"
#include "compiler/semantic/bodies/functions/callback_checker/callback_checker.h"
#include "compiler/semantic/bodies/functions/closure_checker/closure_checker.h"
#include "compiler/semantic/flow/flow_checker/flow_checker.h"
#include "compiler/semantic/infer/return_inferrer/return_inferrer.h"
#include "compiler/semantic/library/result/result_checker/result_checker.h"
#include "compiler/semantic/library/tz_modules/library_calls/library_calls.h"
#include "compiler/semantic/types/destination/destination.h"
#include "compiler/semantic/types/literal_types/literal_types.h"
#include "compiler/semantic/types/type_relations/type_relations.h"
#include "compiler/semantic/types/type_resolver/type_resolver.h"
#include "llvm/ADT/STLExtras.h"

using llvm::cast;
using llvm::dyn_cast;
using llvm::isa;

namespace tz {

const Type *PromiseChecker::awaited(const Type *type) {
  return type->isPromise() ? type->promised() : type;
}

PromiseChecker::PromiseChecker(SemanticState &owner)
    : state(owner) {}

const Type *PromiseChecker::resolverType(const Type *value) {
  std::vector<const Type *> params;
  if (!value->isVoid()) {
    params.push_back(value);
  }

  return Type::function(params, params.size(), false, Type::voidTy());
}

const Type *PromiseChecker::executorValueType(const Expr *executor) {
  auto *fe = dyn_cast<FunctionExpr>(executor);
  if (!fe) {
    return nullptr;
  }
  if (fe->fn->params.empty()) {
    return Type::voidTy();
  }
  llvm::StringRef resolve = fe->fn->params[0].name;
  bool given = false;
  AstWalker::visitExprs(fe->fn->body, [&](const Expr *e) {
    auto *call = dyn_cast<CallExpr>(e);
    auto *name = call ? dyn_cast<NameExpr>(call->callee) : nullptr;
    given |= name && name->name == resolve && !call->args.empty();
  });
  if (!given) {
    return Type::voidTy();
  }
  if (fe->fn->pristine == nullptr) {
    return nullptr;
  }
  FunctionDecl *copy = ReturnInferrer(this->state).freshCopy(fe->fn);
  copy->sym = nullptr;
  auto *trial = this->state.getAst().make<FunctionExpr>(fe->loc(), copy);
  Flow saved = this->state.getFlowState().getFlow();
  bool muted = this->state.getDiagnostics().setMuted(true);
  Signature sig;
  sig.params = {Type::error(), Type::error()};
  sig.ret = Type::voidTy();
  sig.voidResult = true;
  sig.pad = false;
  ClosureChecker(this->state).checkFunctionExpr(trial, &sig);
  this->state.getDiagnostics().setMuted(muted);
  this->state.getFlowState().replaceFlow(std::move(saved));
  const Type *found = nullptr;
  bool unknown = false;
  AstWalker::visitExprs(copy->body, [&](const Expr *e) {
    auto *call = dyn_cast<CallExpr>(e);
    auto *name = call ? dyn_cast<NameExpr>(call->callee) : nullptr;
    if (!name || name->name != resolve || call->args.empty()) {
      return;
    }
    const Type *t = call->args[0]->type;
    if (!t || t->isError() || t->isVoid() || t->isUndefined()) {
      unknown = true;
      return;
    }
    const Type *common = found ? LiteralTypes::unionUnify(found, t) : t;
    if (!common || common->isError()) {
      unknown = true;
    } else {
      found = common;
    }
  });

  return unknown ? nullptr : found;
}

PromiseChecker::ResultValueTypeKnown PromiseChecker::valueType(NewExpr *expr, const CallExpr *call,
                                                               const Type *expected) {
  if (expr->typeArgs.size() > 1) {
    std::string message = ErrorTextPromiseValue::typeArgs();

    return err(PromiseCheckerValueTypeError{
        .code = PromiseCheckerValueTypeErrorCode::TOO_MANY_TYPE_ARGUMENTS,
        .message = message,
    });
  }
  if (expr->typeArgs.size() == 1) {
    const Type *written = TypeResolver(this->state).resolveType(expr->typeArgs[0], true);
    return ok(PromiseChecker::awaited(written));
  }
  if (expected && expected->nonOptional()->isPromise() &&
      !expected->nonOptional()->promised()->isNever()) {
    return ok(expected->nonOptional()->promised());
  }
  if (call->args.size() != 1) {
    return ok(nullptr);
  }

  const Type *inferred = this->executorValueType(call->args[0]);
  if (!inferred) {
    std::string message = ErrorTextPromiseValue::notInferred();

    return err(PromiseCheckerValueTypeError{
        .code = PromiseCheckerValueTypeErrorCode::VALUE_NOT_INFERRED,
        .message = message,
    });
  }

  return ok(inferred);
}

PromiseChecker::ResultExecutorShaped PromiseChecker::executorShape(const Expr *executor) {
  auto *function = dyn_cast<FunctionExpr>(executor);
  if (function && function->fn->params.size() > 1) {
    std::string message = ErrorTextExecutor::rejects();

    return err(PromiseCheckerExecutorShapeError{
        .code = PromiseCheckerExecutorShapeErrorCode::REJECT_PARAMETER,
        .message = message,
    });
  }

  return ok(executor);
}

Expr *PromiseChecker::checkPromiseNew(NewExpr *expr, const Type *expected) {
  this->state.getInventoryStore().markUsesAsync();
  auto *call = this->state.getAst().make<CallExpr>(expr->loc(), expr, expr->args);
  call->target = BuiltinCall{.builtin = Builtin::PromiseNew};
  call->type = Type::error();
  ResultValueTypeKnown known = this->valueType(expr, call, expected);
  if (known.isErr()) {
    this->state.getDiagnostics().error(expr->loc(), known.error().message);
  }
  const Type *value = known.isOk() ? known.value() : Type::error();
  ArgumentChecker::ResultArityMatches arity = ArgumentChecker(this->state).checkArity(call, 1, 1);
  if (arity.isErr()) {
    this->state.getDiagnostics().error(expr->loc(), arity.error().message);
    return call;
  }
  if (value->isError()) {
    Signature loose;
    loose.params = {Type::error(), Type::error()};
    loose.pad = false;
    call->args[0] = isa<FunctionExpr>(call->args[0])
                        ? ClosureChecker(this->state)
                              .checkFunctionExpr(cast<FunctionExpr>(call->args[0]), &loose)
                        : ExprChecker(this->state).checkExpr(call->args[0]);
    return call;
  }
  Signature sig;
  sig.params = {PromiseChecker::resolverType(value)};
  ResultExecutorShaped shaped = PromiseChecker::executorShape(call->args[0]);
  if (shaped.isErr()) {
    llvm::SMLoc rejectLoc = cast<FunctionExpr>(call->args[0])->fn->params[1].loc;
    this->state.getDiagnostics().error(rejectLoc, shaped.error().message);
    sig.params.push_back(Type::error());
  }
  sig.minArgs = sig.params.size();
  sig.ret = Type::voidTy();
  sig.voidResult = true;
  sig.pad = false;
  call->args[0] =
      CallbackChecker(this->state)
          .checkFunctionArg(call->args[0], sig, unsigned(sig.params.size()), "new Promise");
  FlowChecker(this->state).calledFunction();
  call->type = Type::promise(value);

  return call;
}

PromiseChecker::ResultPromiseChained
PromiseChecker::checkPromiseMethod(CallExpr *call, MemberExpr *member, const Type *expected) {
  const Type *type = member->object->type;
  const Type *value = type->promised();
  llvm::StringRef name = member->name;
  static const llvm::StringLiteral resultMethods[] = {"isOk",    "isErr",  "map",       "mapErr",
                                                      "andThen", "orElse", "match",     "unwrapOr",
                                                      "andTee",  "orTee",  "andThrough"};
  if (value->isResult() && llvm::is_contained(resultMethods, name)) {
    auto *object = dyn_cast<NameExpr>(member->object);
    std::string promise = object ? object->name.str() : "p";
    ArgumentChecker(this->state).checkArgsLoosely(call);
    call->type = Type::error();
    std::string message = ErrorTextPromiseMethod::onResult(name, promise);

    return err(PromiseCheckerCheckPromiseMethodError{
        .code = PromiseCheckerCheckPromiseMethodErrorCode::RESULT_METHOD,
        .message = message,
    });
  }
  if (CallChecker(this->state).rejectSpread(call)) {
    return err(PromiseCheckerCheckPromiseMethodError{
        .code = PromiseCheckerCheckPromiseMethodErrorCode::ALREADY_REPORTED,
        .message = "",
    });
  }
  this->state.getInventoryStore().markUsesAsync();
  if (name == "catch") {
    ArgumentChecker(this->state).checkArgsLoosely(call);
    call->type = Type::error();
    std::string message = ErrorTextPromiseMethod::noCatch();

    return err(PromiseCheckerCheckPromiseMethodError{
        .code = PromiseCheckerCheckPromiseMethodErrorCode::NO_CATCH,
        .message = message,
    });
  }
  if (name == "finally") {
    ArgumentChecker(this->state).checkArgsLoosely(call);
    call->type = Type::error();
    std::string message = ErrorTextPromiseMethod::noFinally();

    return err(PromiseCheckerCheckPromiseMethodError{
        .code = PromiseCheckerCheckPromiseMethodErrorCode::NO_FINALLY,
        .message = message,
    });
  }
  if (name == "then" && call->args.size() == 2) {
    ArgumentChecker(this->state).checkArgsLoosely(call);
    call->type = Type::error();
    std::string message = ErrorTextPromiseMethod::thenTwo();

    return err(PromiseCheckerCheckPromiseMethodError{
        .code = PromiseCheckerCheckPromiseMethodErrorCode::THEN_TWO_FUNCTIONS,
        .message = message,
    });
  }
  if (name != "then") {
    ArgumentChecker(this->state).checkArgsLoosely(call);
    std::string message = ErrorTextPromiseMethod::noMethod(name, type->name());

    return err(PromiseCheckerCheckPromiseMethodError{
        .code = PromiseCheckerCheckPromiseMethodErrorCode::NO_SUCH_METHOD,
        .message = message,
    });
  }

  ArgumentChecker::ResultArityMatches arity = ArgumentChecker(this->state).checkArity(call, 1, 1);
  if (arity.isErr()) {
    return err(PromiseCheckerCheckPromiseMethodError{
        .code = PromiseCheckerCheckPromiseMethodErrorCode::WRONG_ARITY,
        .message = arity.error().message,
    });
  }

  const Type *wanted = expected && expected->nonOptional()->isPromise()
                           ? expected->nonOptional()->promised()
                           : nullptr;
  bool failed = false;
  auto callback = [&](Expr *&arg, const Type *param) -> const Type * {
    if (isa<UndefinedExpr>(arg)) {
      arg = ExprChecker(this->state).checkExpr(arg);
      return nullptr;
    }
    Signature sig;
    sig.pad = false;
    if (param && !param->isVoid()) {
      sig.params = {param};
    }
    sig.minArgs = 0;
    if (wanted && isa<FunctionExpr>(arg) && Destination::literalShaped(arg)) {
      sig.ret = wanted;
    }
    arg = CallbackChecker(this->state).checkFunctionArg(arg, sig, 1, name);
    if (!arg->type->isFunction()) {
      failed = true;
      return nullptr;
    }
    if (auto *fe = dyn_cast<FunctionExpr>(arg)) {
      fe->fn->nonEscaping = false;
    }

    return PromiseChecker::awaited(arg->type->ret());
  };
  call->target = BuiltinCall{.builtin = Builtin::PromiseThen};
  const Type *onFulfilled = callback(call->args[0], value);
  const Type *result = onFulfilled ? onFulfilled : value;
  if (failed || result->isError()) {
    return err(PromiseCheckerCheckPromiseMethodError{
        .code = PromiseCheckerCheckPromiseMethodErrorCode::ALREADY_REPORTED,
        .message = "",
    });
  }
  if (result->isNever()) {
    result = value;
  }
  call->type = Type::promise(result);

  return ok(call);
}

}
