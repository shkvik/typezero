#include "compiler/semantic/code/returns/code_returns_registry.h"
#include "compiler/semantic/code/functions/code_functions_registry_errors.h"
#include "compiler/semantic/library/async/async_checker/async_checker.h"

using llvm::isa;

namespace tz {

CodeReturnsRegistry::CodeReturnsRegistry(SemanticState &owner)
    : state(owner) {}

void CodeReturnsRegistry::walkReturn(const WalkReturn &task) {
  ReturnStmt *returnStmt = task.returnStmt;
  auto inside = this->checkInsideFunction();
  if (inside.isErr()) {
    this->state.diagnostics.error(returnStmt->loc(), inside.error().message);
    return this->startStrayValue(returnStmt);
  }
  this->state.walk.tasks.pushTask(
      AfterUnlistenedErrors{.returnStmt = returnStmt, .function = inside.value()});

  return this->state.walk.tasks.pushTask(ReportUnlistenedErrors{});
}

void CodeReturnsRegistry::startStrayValue(ReturnStmt *returnStmt) {
  if (returnStmt->value != nullptr) {
    this->state.walk.tasks.pushTask(CheckExprSlot{.slot = &returnStmt->value, .expected = nullptr});
  }
}

void CodeReturnsRegistry::afterUnlistenedErrors(const AfterUnlistenedErrors &task) {
  ReturnStmt *returnStmt = task.returnStmt;
  const FuncSymbol *function = task.function;
  const FunctionDecl *functionDecl = function->decl;
  if (functionDecl != nullptr && functionDecl->isConstructor()) {
    return this->startConstructorReturn(returnStmt, function);
  }
  auto awaited = this->checkAwaitNotReturned(returnStmt, function);
  if (awaited.isErr()) {
    this->state.diagnostics.error(returnStmt->value->loc(), awaited.error().message);
  }
  const auto *returned = this->declaredReturn(function);
  if (!function->hasReturnType()) {
    return this->startInferredReturn(returnStmt);
  }
  if (returnStmt->value == nullptr) {
    return this->walkBareReturn(returnStmt, function, returned);
  }
  bool isAsyncGenerator =
      functionDecl != nullptr && functionDecl->isAsync && functionDecl->isGenerator;
  const Type *hint = returned->isVoid() ? nullptr : returned;
  if (isAsyncGenerator) {
    this->state.walk.tasks.pushTask(AfterAsyncGeneratorReturn{
        .returnStmt = returnStmt,
        .function = function,
        .returned = returned,
    });
    return this->state.walk.tasks.pushTask(
        CheckExprSlot{.slot = &returnStmt->value, .expected = hint});
  }
  bool isTested = returned->isBool();
  this->state.walk.tasks.pushTask(AfterReturnValue{
      .returnStmt = returnStmt,
      .function = function,
      .returned = returned,
  });

  return this->startReturnValue(returnStmt, hint, isTested);
}

void CodeReturnsRegistry::startReturnValue(ReturnStmt *returnStmt, const Type *hint,
                                           bool isTested) {
  if (!this->state.flow.predicates.isBuildingPredicate()) {
    return this->state.walk.tasks.pushTask(
        CheckExprSlot{.slot = &returnStmt->value, .expected = hint});
  }
  if (isTested) {
    this->state.walk.tasks.pushTask(AfterTestedReturn{.returnStmt = returnStmt});
    return this->state.walk.tasks.pushTask(
        StartBranch{.expr = returnStmt->value, .use = TruthUse::Condition});
  }
  this->state.walk.tasks.pushTask(AfterPredicateReturn{.returnStmt = returnStmt});

  return this->state.walk.tasks.pushTask(
      CheckExprSlot{.slot = &returnStmt->value, .expected = hint});
}

void CodeReturnsRegistry::afterTestedReturn(const AfterTestedReturn &task) {
  ReturnStmt *returnStmt = task.returnStmt;
  returnStmt->value = this->state.walk.tasks.takeChecked();
  auto flows = this->state.walk.tasks.takeConditionFlows();
  auto joined = this->state.flow.joins.join(flows.whenTrue, flows.whenFalse);
  this->state.walk.flow.replaceFlow(std::move(joined));

  return this->state.walk.tasks.pushTask(NotePredicateExit{
      .value = returnStmt->value,
      .whenTrue = std::move(flows.whenTrue),
      .whenFalse = std::move(flows.whenFalse),
  });
}

void CodeReturnsRegistry::afterPredicateReturn(const AfterPredicateReturn &task) {
  const Expr *value = task.returnStmt->value;
  if (!value->type->isBool()) {
    return;
  }
  const auto &flow = this->state.walk.flow.getFlow();
  this->state.walk.tasks.pushConditionFlows(ConditionFlows{
      .whenTrue = flow,
      .whenFalse = flow,
      .constant = std::nullopt,
  });
  this->state.walk.tasks.pushTask(AfterPredicateFacts{.returnStmt = task.returnStmt});

  return this->state.walk.tasks.pushTask(AddConditionFacts{.cond = value});
}

void CodeReturnsRegistry::afterPredicateFacts(const AfterPredicateFacts &task) {
  auto flows = this->state.walk.tasks.takeConditionFlows();

  return this->state.walk.tasks.pushTask(NotePredicateExit{
      .value = task.returnStmt->value,
      .whenTrue = std::move(flows.whenTrue),
      .whenFalse = std::move(flows.whenFalse),
  });
}

void CodeReturnsRegistry::afterReturnValue(const AfterReturnValue &task) {
  ReturnStmt *returnStmt = task.returnStmt;
  const FuncSymbol *function = task.function;
  const FunctionDecl *functionDecl = function->decl;
  bool isPromised = functionDecl != nullptr && functionDecl->isAsync &&
                    function->getKnownReturnType()->isPromise() &&
                    returnStmt->value->type->isPromise();
  if (isPromised) {
    return this->walkPromiseReturn(returnStmt, function, task.returned);
  }
  if (task.returned->isVoid()) {
    return this->walkVoidReturn(returnStmt, function);
  }
  this->state.diagnostics.pushList(DiagnosticRoute::PARENT);
  this->state.walk.tasks.pushTask(AfterValueCoerce{.returnStmt = returnStmt, .function = function});

  return this->state.walk.tasks.pushTask(
      CoerceSlot{.slot = &returnStmt->value, .to = task.returned, .what = ""});
}

void CodeReturnsRegistry::afterValueCoerce(const AfterValueCoerce &task) {
  auto &diagnostics = this->state.diagnostics;
  auto coerceDiagnostics = diagnostics.popList();
  bool isReachingProgram = diagnostics.getCurrentDestination() == DiagnosticRoute::PROGRAM;
  const FunctionDecl *functionDecl = task.function->decl;
  if (Diagnostics::hasErrors(coerceDiagnostics) && isReachingProgram) {
    return this->state.walk.tasks.pushTask(NoteInheritedReturn{.functionDecl = functionDecl});
  }
  if (functionDecl == nullptr) {
    return;
  }

  return this->state.walk.tasks.pushTask(
      CheckByteCallbackResult{.functionDecl = functionDecl, .value = task.returnStmt->value});
}

void CodeReturnsRegistry::walkVoidReturn(ReturnStmt *returnStmt, const FuncSymbol *function) {
  auto empty = this->checkVoidReturn(returnStmt->value->type, function);
  if (empty.isErr()) {
    this->state.diagnostics.error(returnStmt->value->loc(), empty.error().message);
    return this->state.walk.tasks.pushTask(NoteInheritedReturn{.functionDecl = function->decl});
  }
  if (!function->discardsResult) {
    return;
  }

  return this->state.walk.tasks.pushTask(CheckUsedCallbackValue{.expr = returnStmt->value});
}

void CodeReturnsRegistry::startConstructorReturn(ReturnStmt *returnStmt,
                                                 const FuncSymbol *function) {
  this->state.walk.tasks.pushTask(
      AfterConstructorValue{.returnStmt = returnStmt, .function = function});

  return this->startStrayValue(returnStmt);
}

void CodeReturnsRegistry::afterConstructorValue(const AfterConstructorValue &task) {
  const ReturnStmt *returnStmt = task.returnStmt;
  auto exits = this->checkConstructorReturn(returnStmt, task.function);
  if (exits.isErr()) {
    bool isValued =
        exits.error().code == CodeReturnsRegistryConstructorReturnErrorCode::VALUE_RETURNED;
    llvm::SMLoc at = isValued ? returnStmt->value->loc() : returnStmt->loc();
    return this->state.diagnostics.error(at, exits.error().message);
  }
  bool isInitializer = this->state.walk.self.getInitializer() == task.function->decl;
  if (isInitializer) {
    return this->state.walk.tasks.pushTask(CheckFieldsSet{.loc = returnStmt->loc()});
  }
}

CodeReturnsRegistry::ResultConstructorExits
CodeReturnsRegistry::checkConstructorReturn(const ReturnStmt *returnStmt,
                                            const FuncSymbol *function) const {
  if (returnStmt->value != nullptr) {
    auto message = ErrorTextConstructorReturn::value();

    return err(CodeReturnsRegistryConstructorReturnError{
        .code = CodeReturnsRegistryConstructorReturnErrorCode::VALUE_RETURNED,
        .message = message,
    });
  }
  bool isInitializer = this->state.walk.self.getInitializer() == function->decl;
  if (isInitializer && this->state.walk.flow.isBeforeSuper()) {
    auto message = ErrorTextConstructorReturn::early();

    return err(CodeReturnsRegistryConstructorReturnError{
        .code = CodeReturnsRegistryConstructorReturnErrorCode::BEFORE_SUPER,
        .message = message,
    });
  }

  return ok(function);
}

void CodeReturnsRegistry::startInferredReturn(ReturnStmt *returnStmt) {
  if (returnStmt->value == nullptr) {
    return this->state.walk.functions.addCollectedReturn(returnStmt);
  }
  bool isShaped = this->state.inference.destinations.isLiteralShaped(returnStmt->value);
  const Type *hint = isShaped ? this->earlierReturnShape() : nullptr;
  bool isTested = this->state.flow.functions.isLookingBoolean(returnStmt->value);
  this->state.walk.tasks.pushTask(AfterInferredReturn{.returnStmt = returnStmt});

  return this->startReturnValue(returnStmt, hint, isTested);
}

void CodeReturnsRegistry::afterInferredReturn(const AfterInferredReturn &task) {
  return this->state.walk.functions.addCollectedReturn(task.returnStmt);
}

void CodeReturnsRegistry::afterAsyncGeneratorReturn(const AfterAsyncGeneratorReturn &task) {
  const Expr *value = task.returnStmt->value;
  const Type *valueType = value->type;
  const Type *given = valueType->isError() ? nullptr : AsyncChecker::awaitedType(valueType);
  auto gives = this->checkGeneratorReturn(given, task.returned, task.function);
  if (gives.isErr()) {
    return this->state.diagnostics.error(value->loc(), gives.error().message);
  }
}

CodeReturnsRegistry::ResultInsideOfFunction CodeReturnsRegistry::checkInsideFunction() const {
  const auto *function = this->state.walk.functions.getCurrentFunction();
  if (function == nullptr) {
    auto message = ErrorTextReturnPlace::outside();

    return err(CodeReturnsRegistryEnclosingFunctionError{
        .code = CodeReturnsRegistryEnclosingFunctionErrorCode::OUTSIDE_FUNCTION,
        .message = message,
    });
  }

  return ok(function);
}

CodeReturnsRegistry::ResultAwaitNotReturned
CodeReturnsRegistry::checkAwaitNotReturned(const ReturnStmt *returnStmt,
                                           const FuncSymbol *function) const {
  const FunctionDecl *functionDecl = function->decl;
  bool isAsyncValue = functionDecl != nullptr && functionDecl->isAsync &&
                      !functionDecl->isGenerator && returnStmt->value != nullptr;
  bool isAwaited = isAsyncValue && isa<AwaitExpr>(returnStmt->value);
  if (isAwaited && functionDecl->concise()) {
    auto message = ErrorTextReturnAwait::concise();

    return err(CodeReturnsRegistryAwaitReturnedError{
        .code = CodeReturnsRegistryAwaitReturnedErrorCode::CONCISE_AWAIT,
        .message = message,
    });
  }
  if (isAwaited) {
    auto message = ErrorTextReturnAwait::returned();

    return err(CodeReturnsRegistryAwaitReturnedError{
        .code = CodeReturnsRegistryAwaitReturnedErrorCode::RETURN_AWAIT,
        .message = message,
    });
  }

  return ok(returnStmt);
}

CodeReturnsRegistry::ResultValueNotRequired
CodeReturnsRegistry::checkValueNotRequired(const Type *returned, const FuncSymbol *function) const {
  if (!returned->isVoid() && !returned->isError() && !returned->isOptional()) {
    auto label = ErrorTextFunctionName::label(function->name);
    auto message = ErrorTextReturnValue::missing(label, returned->name());

    return err(CodeReturnsRegistryValueRequiredError{
        .code = CodeReturnsRegistryValueRequiredErrorCode::MISSING_VALUE,
        .message = message,
    });
  }

  return ok(returned);
}

CodeReturnsRegistry::ResultGeneratorReturns
CodeReturnsRegistry::checkGeneratorReturn(const Type *given, const Type *returned,
                                          const FuncSymbol *function) const {
  bool isGiven = given != nullptr && !given->isUndefined() && !given->isVoid();
  bool isWrong = isGiven && (returned->isVoid() ||
                             !this->state.types.relations.isImplicitlyConvertible(given, returned));
  if (isWrong) {
    auto label = ErrorTextFunctionName::label(function->name);
    auto message = ErrorTextReturnValue::wrong(label, returned->name(), given->name());

    return err(CodeReturnsRegistryGeneratorReturnError{
        .code = CodeReturnsRegistryGeneratorReturnErrorCode::WRONG_TYPE,
        .message = message,
    });
  }

  return ok(returned);
}

CodeReturnsRegistry::ResultPromiseTypeMatch
CodeReturnsRegistry::checkPromiseReturn(const Type *given, const Type *returned,
                                        const FuncSymbol *function) const {
  if (given != returned && !given->isNever()) {
    auto label = ErrorTextFunctionName::label(function->name);
    auto message = ErrorTextReturnValue::promise(given->name(), label, returned->name());

    return err(CodeReturnsRegistryPromiseReturnError{
        .code = CodeReturnsRegistryPromiseReturnErrorCode::OTHER_PROMISE_TYPE,
        .message = message,
    });
  }

  return ok(given);
}

CodeReturnsRegistry::ResultVoidReturnsEmpty
CodeReturnsRegistry::checkVoidReturn(const Type *type, const FuncSymbol *function) const {
  if (!type->isVoid() && !type->isError() && !type->isUndefined() && !function->discardsResult) {
    auto label = ErrorTextFunctionName::label(function->name);
    auto message = ErrorTextReturnValue::notVoid(label, type->name());

    return err(CodeReturnsRegistryVoidReturnError{
        .code = CodeReturnsRegistryVoidReturnErrorCode::VALUE_FROM_VOID,
        .message = message,
    });
  }

  return ok(type);
}

void CodeReturnsRegistry::walkBareReturn(const ReturnStmt *returnStmt, const FuncSymbol *function,
                                         const Type *returned) {
  auto bare = this->checkValueNotRequired(returned, function);
  if (bare.isErr()) {
    this->state.diagnostics.error(returnStmt->loc(), bare.error().message);
  }
}

const Type *CodeReturnsRegistry::declaredReturn(const FuncSymbol *function) const {
  const FunctionDecl *functionDecl = function->decl;
  bool isDeclared = function->hasReturnType() && functionDecl != nullptr;
  const Type *known = function->getKnownReturnType();
  bool isGenerator =
      isDeclared && functionDecl->isGenerator && (known->isIterator() || known->isAsyncIterator());
  if (isGenerator) {
    return known->returned();
  }
  bool isPromised = isDeclared && functionDecl->isAsync && known->isPromise();
  if (isPromised) {
    return known->promised();
  }

  return known;
}

const Type *CodeReturnsRegistry::earlierReturnShape() const {
  for (const ReturnStmt *earlier : this->state.walk.functions.getCollectedReturns()) {
    if (this->isShapedReturn(earlier)) {
      return earlier->value->type;
    }
  }

  return nullptr;
}

bool CodeReturnsRegistry::isShapedReturn(const ReturnStmt *earlier) const {
  if (earlier->value == nullptr || earlier->value->type->isError()) {
    return false;
  }
  const Type *type = earlier->value->type;

  return type->holdsObjects() || type->isResult();
}

void CodeReturnsRegistry::walkPromiseReturn(ReturnStmt *returnStmt, const FuncSymbol *function,
                                            const Type *returned) {
  const Type *given = returnStmt->value->type->promised();
  auto match = this->checkPromiseReturn(given, returned, function);
  if (match.isErr()) {
    this->state.diagnostics.error(returnStmt->value->loc(), match.error().message);
  }
}

}
