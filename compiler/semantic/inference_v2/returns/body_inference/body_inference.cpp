#include "compiler/semantic/inference_v2/returns/body_inference/body_inference.h"
#include "compiler/semantic/library/async/async_checker/async_checker.h"

namespace tz {

BodyInference::BodyInference(SemanticState &owner, ReturnRules &sharedReturnRules)
    : state(owner),
      returnRules(sharedReturnRules) {}

void BodyInference::inferFromBody(const InferFromBody &task) {
  FunctionDecl *functionDecl = task.functionDecl;
  FuncSymbol *function = functionDecl->sym;
  auto &tasks = this->state.walk.tasks;
  auto &functions = this->state.walk.functions;
  auto &returns = this->state.inference.returns;
  tasks.pushTask(FinishInferFromBody{
      .functionDecl = functionDecl,
      .outerReturns = functions.getReturnCollector(),
      .outerYields = functions.getYieldCollector(),
  });
  CollectedReturns &collected = returns.openCollection();
  functions.rememberReturnCollector(&collected.returns);
  functions.rememberYieldCollector(&collected.yields);
  function->inferring = true;
  returns.enterInference(function);

  return tasks.pushTask(WalkFunctionBody{.functionDecl = functionDecl});
}

void BodyInference::finishInferFromBody(const FinishInferFromBody &task) {
  FunctionDecl *functionDecl = task.functionDecl;
  FuncSymbol *function = functionDecl->sym;
  auto &functions = this->state.walk.functions;
  auto &returns = this->state.inference.returns;
  returns.leaveInference();
  function->inferring = false;
  auto collected = returns.takeCollection();
  functions.rememberReturnCollector(task.outerReturns);
  functions.rememberYieldCollector(task.outerYields);
  bool isFallingOff = !this->state.flow.reachability.isAlwaysReturning(functionDecl->body);
  const auto *type = this->returnRules.collectedType(functionDecl, function, collected,
                                                     isFallingOff, ReturnErrors::REPORTED);
  function->setReturnType(type != nullptr ? type : Type::error());
  if (functionDecl->isAsync && !functionDecl->isGenerator) {
    return this->state.walk.tasks.pushTask(
        FinishAsyncReturns{.function = function, .returns = collected.returns});
  }

  return this->convertReturns(functionDecl, collected);
}

void BodyInference::convertReturns(const FunctionDecl *functionDecl,
                                   const CollectedReturns &collected) {
  auto &tasks = this->state.walk.tasks;
  const Type *known = functionDecl->sym->getKnownReturnType();
  bool isAsync = functionDecl->isAsync;
  bool isIterating = known->isIterator() || known->isAsyncIterator();
  if (isIterating) {
    const Type *yielded = known->yielded();
    for (YieldExpr *yield : llvm::reverse(collected.yields)) {
      if (this->isConvertibleYield(yield, isAsync)) {
        tasks.pushTask(ConvertSlot{.slot = &yield->value, .to = yielded});
      }
    }
  }
  const Type *returned = functionDecl->isGenerator && isIterating ? known->returned() : known;
  if (returned->isError() || returned->isVoid()) {
    return;
  }
  for (ReturnStmt *returnStmt : llvm::reverse(collected.returns)) {
    if (this->isConvertibleReturn(returnStmt, isAsync)) {
      tasks.pushTask(ConvertSlot{.slot = &returnStmt->value, .to = returned});
    }
  }
}

bool BodyInference::isSettled(const Type *type) const {
  const auto *awaited = AsyncChecker::awaitedType(type);

  return awaited == type;
}

bool BodyInference::isConvertibleReturn(const ReturnStmt *returnStmt, bool isAsync) const {
  const Expr *value = returnStmt->value;
  if (value == nullptr || value->type->isError()) {
    return false;
  }
  if (!isAsync) {
    return true;
  }

  return this->isSettled(value->type) && !value->type->isUndefined();
}

bool BodyInference::isConvertibleYield(const YieldExpr *yield, bool isAsync) const {
  if (yield->delegate || yield->value == nullptr || yield->value->type->isError()) {
    return false;
  }
  if (!isAsync) {
    return true;
  }

  return this->isSettled(yield->value->type);
}

}
