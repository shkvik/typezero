#include "compiler/semantic/library/async/promise_checker/promise_checker.h"
#include "compiler/program/ast_walker/ast_walker.h"
#include "compiler/program/library/async/promise_method_table/promise_method_table.h"
#include "compiler/program/library/result/result_method_table/result_method_table.h"
#include "compiler/semantic/bodies/expr/argument_checker/argument_checker.h"
#include "compiler/semantic/bodies/expr/call_checker/call_checker.h"
#include "compiler/semantic/bodies/expr/expr_checker/expr_checker.h"
#include "compiler/semantic/bodies/functions/callback_checker/callback_checker.h"
#include "compiler/semantic/bodies/functions/closure_checker/closure_checker.h"
#include "compiler/semantic/flow/flow_checker/flow_checker.h"
#include "compiler/semantic/flow/placed_nodes/placed_nodes.h"
#include "compiler/semantic/infer/return_inferrer/return_inferrer.h"
#include "compiler/semantic/library/result/result_checker/result_checker.h"
#include "compiler/semantic/library/tz_modules/library_calls/library_calls.h"
#include "compiler/semantic/semantic_parts/semantic_parts.h"
#include "compiler/semantic/types/destination/destination.h"
#include "compiler/semantic/types/literal_types/literal_types.h"
#include "compiler/semantic/types/type_relations/type_relations.h"
#include "compiler/semantic/types/type_resolver/type_resolver.h"
#include "llvm/ADT/STLExtras.h"

using llvm::cast;
using llvm::dyn_cast;
using llvm::isa;

namespace tz {

namespace {

constexpr llvm::StringLiteral kNewPromiseLabel = "new Promise";
constexpr llvm::StringLiteral kPromisePlaceholder = "p";

}

PromiseChecker::PromiseChecker(SemanticState &owner, SemanticParts &all)
    : state(owner) {
  this->callChecker = &all.callChecker;
  this->exprChecker = &all.exprChecker;
  this->flowChecker = &all.flowChecker;
  this->destination = &all.destination;
  this->typeResolver = &all.typeResolver;
  this->closureChecker = &all.closureChecker;
  this->returnInferrer = &all.returnInferrer;
  this->argumentChecker = &all.argumentChecker;
  this->callbackChecker = &all.callbackChecker;
}

const Type *PromiseChecker::withoutPromise(const Type *type) const {
  return type->isPromise() ? type->promised() : type;
}

const Type *PromiseChecker::expectedPromised(const Type *expected) const {
  if (expected == nullptr || !expected->nonOptional()->isPromise()) {
    return nullptr;
  }

  return expected->nonOptional()->promised();
}

const Type *PromiseChecker::resolverType(const Type *value) const {
  auto params = std::vector<const Type *>();
  if (!value->isVoid()) {
    params.push_back(value);
  }

  return Type::function(params, params.size(), false, Type::voidTy());
}

const CallExpr *PromiseChecker::resolveCall(const Expr *expr, llvm::StringRef resolve) const {
  auto *call = dyn_cast<CallExpr>(expr);
  if (call == nullptr || call->args.empty()) {
    return nullptr;
  }
  auto *name = dyn_cast<NameExpr>(call->callee);
  if (name == nullptr || name->name != resolve) {
    return nullptr;
  }

  return call;
}

bool PromiseChecker::isResolvingWithValue(const Stmt *body, llvm::StringRef resolve) const {
  auto nodes = PlacedNodes::collect({body}, {});
  for (const PlacedExpr &placed : nodes.getExprs()) {
    if (this->resolveCall(placed.expr, resolve) != nullptr) {
      return true;
    }
  }

  return false;
}

const Type *PromiseChecker::executorValueType(const Expr *executor) {
  auto *functionExpr = dyn_cast<FunctionExpr>(executor);
  if (functionExpr == nullptr) {
    return nullptr;
  }
  const FunctionDecl *executorDecl = functionExpr->fn;
  if (executorDecl->params.empty()) {
    return Type::voidTy();
  }
  llvm::StringRef resolve = executorDecl->params[0].name;
  if (!this->isResolvingWithValue(executorDecl->body, resolve)) {
    return Type::voidTy();
  }
  if (executorDecl->pristine == nullptr) {
    return nullptr;
  }
  const auto *trial = this->walkedTrial(functionExpr);

  return this->resolvedValueType(trial->body, resolve);
}

const FunctionDecl *PromiseChecker::walkedTrial(const FunctionExpr *functionExpr) {
  auto *copy = this->returnInferrer->freshCopy(functionExpr->fn);
  copy->sym = nullptr;
  auto *trial = this->state.getAst().make<FunctionExpr>(functionExpr->loc(), copy);
  auto saved = this->state.getFlowState().getFlow();
  auto signature = Signature{
      .params = {Type::error(), Type::error()},
      .ret = Type::voidTy(),
      .voidResult = true,
      .pad = false,
  };
  this->state.getDiagnostics().pushList(DiagnosticRoute::DISCARD);
  this->closureChecker->checkFunctionExpr(trial, &signature);
  this->state.getDiagnostics().popList();
  this->state.getFlowState().replaceFlow(std::move(saved));

  return copy;
}

const Type *PromiseChecker::resolvedValueType(const Stmt *body, llvm::StringRef resolve) const {
  const Type *found = nullptr;
  bool isUnknown = false;
  for (const CallExpr *call : this->resolveCalls(body, resolve)) {
    const auto *joined = this->joinedValue(found, call->args[0]->type);
    if (joined == nullptr) {
      isUnknown = true;
    } else {
      found = joined;
    }
  }

  return isUnknown ? nullptr : found;
}

std::vector<const CallExpr *> PromiseChecker::resolveCalls(const Stmt *body,
                                                           llvm::StringRef resolve) const {
  auto calls = std::vector<const CallExpr *>();
  for (const Expr *expr : AstWalker::exprsIn(body, true)) {
    const auto *call = this->resolveCall(expr, resolve);
    if (call != nullptr) {
      calls.push_back(call);
    }
  }

  return calls;
}

const Type *PromiseChecker::joinedValue(const Type *found, const Type *type) const {
  if (type == nullptr || type->isError() || type->isVoid() || type->isUndefined()) {
    return nullptr;
  }
  if (found == nullptr) {
    return type;
  }
  const auto *common = LiteralTypes::unionUnify(found, type);
  if (common == nullptr || common->isError()) {
    return nullptr;
  }

  return common;
}

PromiseChecker::ResultValueTypeKnown PromiseChecker::valueType(NewExpr *expr, const CallExpr *call,
                                                               const Type *expected) {
  if (expr->typeArgs.size() > 1) {
    auto message = ErrorTextPromiseValue::typeArgs();

    return err(PromiseCheckerValueTypeError{
        .code = PromiseCheckerValueTypeErrorCode::TOO_MANY_TYPE_ARGUMENTS,
        .message = message,
    });
  }
  if (expr->typeArgs.size() == 1) {
    const auto *written = this->typeResolver->resolveTypeOrVoid(expr->typeArgs[0]);
    const auto *value = this->withoutPromise(written);

    return ok(value);
  }
  const Type *wanted = this->expectedPromised(expected);
  if (wanted != nullptr && !wanted->isNever()) {
    return ok(wanted);
  }
  if (call->args.size() != 1) {
    return ok<const Type *>(nullptr);
  }
  const Type *inferred = this->executorValueType(call->args[0]);
  if (inferred == nullptr) {
    auto message = ErrorTextPromiseValue::notInferred();

    return err(PromiseCheckerValueTypeError{
        .code = PromiseCheckerValueTypeErrorCode::VALUE_NOT_INFERRED,
        .message = message,
    });
  }

  return ok(inferred);
}

PromiseChecker::ResultExecutorShaped PromiseChecker::executorShape(const Expr *executor) const {
  auto *function = dyn_cast<FunctionExpr>(executor);
  if (function != nullptr && function->fn->params.size() > 1) {
    auto message = ErrorTextExecutor::rejects();

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
  auto known = this->valueType(expr, call, expected);
  if (known.isErr()) {
    this->state.getDiagnostics().error(expr->loc(), known.error().message);
  }
  const Type *value = known.isOk() ? known.value() : Type::error();
  auto arity = this->argumentChecker->checkArity(call, 1, 1);
  if (arity.isErr()) {
    this->state.getDiagnostics().error(expr->loc(), arity.error().message);

    return call;
  }
  if (value->isError()) {
    call->args[0] = this->looseExecutor(call->args[0]);

    return call;
  }
  call->args[0] = this->typedExecutor(call->args[0], value);
  this->flowChecker->calledFunction();
  call->type = Type::promise(value);

  return call;
}

Expr *PromiseChecker::looseExecutor(Expr *executor) {
  auto *functionExpr = dyn_cast<FunctionExpr>(executor);
  if (functionExpr == nullptr) {
    return this->exprChecker->checkExpr(executor);
  }
  auto loose = Signature{
      .params = {Type::error(), Type::error()},
      .pad = false,
  };

  return this->closureChecker->checkFunctionExpr(functionExpr, &loose);
}

Expr *PromiseChecker::typedExecutor(Expr *executor, const Type *value) {
  const auto *resolver = this->resolverType(value);
  auto params = std::vector<const Type *>{resolver};
  auto shaped = this->executorShape(executor);
  if (shaped.isErr()) {
    llvm::SMLoc rejectLoc = cast<FunctionExpr>(executor)->fn->params[1].loc;
    this->state.getDiagnostics().error(rejectLoc, shaped.error().message);
    params.push_back(Type::error());
  }
  auto signature = Signature{
      .params = params,
      .minArgs = params.size(),
      .ret = Type::voidTy(),
      .voidResult = true,
      .pad = false,
  };
  auto primary = unsigned(params.size());

  return this->callbackChecker->checkFunctionArg(executor, signature, primary, kNewPromiseLabel);
}

PromiseChecker::ResultPromiseChained
PromiseChecker::checkPromiseMethod(CallExpr *call, MemberExpr *member, const Type *expected) {
  auto plain = this->checkNotResultMethod(call, member);
  if (plain.isErr()) {
    return err(plain.error());
  }
  if (this->callChecker->rejectSpread(call)) {
    return err(PromiseCheckerCheckPromiseMethodError{
        .code = PromiseCheckerCheckPromiseMethodErrorCode::ALREADY_REPORTED,
        .message = "",
    });
  }
  this->state.getInventoryStore().markUsesAsync();
  auto chaining = this->checkIsThen(call, member);
  if (chaining.isErr()) {
    return err(chaining.error());
  }
  auto arity = this->argumentChecker->checkArity(call, 1, 1);
  if (arity.isErr()) {
    return err(PromiseCheckerCheckPromiseMethodError{
        .code = PromiseCheckerCheckPromiseMethodErrorCode::WRONG_ARITY,
        .message = arity.error().message,
    });
  }

  return this->chainThen(call, member, expected);
}

PromiseChecker::ResultMethodAccepted
PromiseChecker::checkNotResultMethod(CallExpr *call, const MemberExpr *member) {
  const Type *value = member->object->type->promised();
  const auto *resultMethod = ResultMethodTable::find(member->name);
  if (value->isResult() && resultMethod != nullptr) {
    auto *object = dyn_cast<NameExpr>(member->object);
    std::string promise = object != nullptr ? object->name.str() : kPromisePlaceholder.str();
    this->argumentChecker->walkArgsLoosely(call);
    call->type = Type::error();
    auto message = ErrorTextPromiseMethod::onResult(member->name, promise);

    return err(PromiseCheckerCheckPromiseMethodError{
        .code = PromiseCheckerCheckPromiseMethodErrorCode::RESULT_METHOD,
        .message = message,
    });
  }

  return ok();
}

PromiseChecker::ResultMethodAccepted PromiseChecker::checkIsThen(CallExpr *call,
                                                                 const MemberExpr *member) {
  auto method = PromiseMethodTable::find(member->name);
  if (method == PromiseMethod::CATCH) {
    this->argumentChecker->walkArgsLoosely(call);
    call->type = Type::error();
    auto message = ErrorTextPromiseMethod::noCatch();

    return err(PromiseCheckerCheckPromiseMethodError{
        .code = PromiseCheckerCheckPromiseMethodErrorCode::NO_CATCH,
        .message = message,
    });
  }
  if (method == PromiseMethod::FINALLY) {
    this->argumentChecker->walkArgsLoosely(call);
    call->type = Type::error();
    auto message = ErrorTextPromiseMethod::noFinally();

    return err(PromiseCheckerCheckPromiseMethodError{
        .code = PromiseCheckerCheckPromiseMethodErrorCode::NO_FINALLY,
        .message = message,
    });
  }
  if (method == PromiseMethod::THEN && call->args.size() == 2) {
    this->argumentChecker->walkArgsLoosely(call);
    call->type = Type::error();
    auto message = ErrorTextPromiseMethod::thenTwo();

    return err(PromiseCheckerCheckPromiseMethodError{
        .code = PromiseCheckerCheckPromiseMethodErrorCode::THEN_TWO_FUNCTIONS,
        .message = message,
    });
  }
  if (method != PromiseMethod::THEN) {
    this->argumentChecker->walkArgsLoosely(call);
    auto message = ErrorTextPromiseMethod::noMethod(member->name, member->object->type->name());

    return err(PromiseCheckerCheckPromiseMethodError{
        .code = PromiseCheckerCheckPromiseMethodErrorCode::NO_SUCH_METHOD,
        .message = message,
    });
  }

  return ok();
}

PromiseChecker::ResultPromiseChained
PromiseChecker::chainThen(CallExpr *call, const MemberExpr *member, const Type *expected) {
  const Type *value = member->object->type->promised();
  call->target = BuiltinCall{.builtin = Builtin::PromiseThen};
  auto returned = this->checkThenCallback(call, member, expected);
  if (returned.isErr()) {
    return err(returned.error());
  }
  const Type *result = returned.value() != nullptr ? returned.value() : value;
  if (result->isError()) {
    return err(PromiseCheckerCheckPromiseMethodError{
        .code = PromiseCheckerCheckPromiseMethodErrorCode::ALREADY_REPORTED,
        .message = "",
    });
  }
  const Type *settled = result->isNever() ? value : result;
  call->type = Type::promise(settled);

  return ok(call);
}

PromiseChecker::ResultThenReturnType
PromiseChecker::checkThenCallback(CallExpr *call, const MemberExpr *member, const Type *expected) {
  if (isa<UndefinedExpr>(call->args[0])) {
    call->args[0] = this->exprChecker->checkExpr(call->args[0]);

    return ok<const Type *>(nullptr);
  }
  auto signature = this->thenSignature(call->args[0], member, expected);
  call->args[0] =
      this->callbackChecker->checkFunctionArg(call->args[0], signature, 1, member->name);
  if (!call->args[0]->type->isFunction()) {
    return err(PromiseCheckerCheckPromiseMethodError{
        .code = PromiseCheckerCheckPromiseMethodErrorCode::ALREADY_REPORTED,
        .message = "",
    });
  }
  auto *functionExpr = dyn_cast<FunctionExpr>(call->args[0]);
  if (functionExpr != nullptr) {
    functionExpr->fn->nonEscaping = false;
  }
  const auto *returned = this->withoutPromise(call->args[0]->type->ret());

  return ok(returned);
}

Signature PromiseChecker::thenSignature(const Expr *callback, const MemberExpr *member,
                                        const Type *expected) const {
  auto signature = Signature{
      .params = {},
      .minArgs = 0,
      .ret = nullptr,
      .voidResult = false,
      .pad = false,
  };
  const Type *value = member->object->type->promised();
  if (value != nullptr && !value->isVoid()) {
    signature.params = {value};
  }
  const Type *wanted = this->expectedPromised(expected);
  if (wanted != nullptr && isa<FunctionExpr>(callback) &&
      this->destination->isLiteralShaped(callback)) {
    signature.ret = wanted;
  }

  return signature;
}

}
