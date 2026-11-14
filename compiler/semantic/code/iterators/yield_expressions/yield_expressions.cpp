#include "compiler/semantic/code/iterators/yield_expressions/yield_expressions.h"
#include "compiler/semantic/code/functions/code_functions_registry_errors.h"
#include "compiler/semantic/library/async/async_checker/async_checker.h"

namespace tz {

YieldExpressions::YieldExpressions(SemanticState &owner)
    : state(owner) {}

void YieldExpressions::checkYield(const CheckYield &task) {
  YieldExpr *yield = task.yield;
  yield->type = Type::error();
  auto *function = this->state.walk.functions.getCurrentFunction();
  bool isInsideGenerator =
      function != nullptr && function->decl != nullptr && function->decl->isGenerator;
  if (!isInsideGenerator) {
    return this->startStrayYield(yield);
  }
  auto shape = this->yieldShape(yield, function);
  this->state.walk.tasks.pushTask(FinishYield{.yield = yield, .shape = shape});
  if (yield->delegate) {
    return this->startDelegate(yield, shape);
  }

  return this->startYieldValue(yield, shape);
}

void YieldExpressions::startStrayYield(YieldExpr *yield) {
  this->state.walk.tasks.pushChecked(yield);
  if (yield->value != nullptr) {
    this->state.walk.tasks.pushTask(CheckExprSlot{.slot = &yield->value, .expected = nullptr});
  }
}

void YieldExpressions::startDelegate(YieldExpr *yield, const YieldShape &shape) {
  this->state.walk.tasks.pushTask(AfterYieldDelegate{.yield = yield, .shape = shape});
  this->state.walk.tasks.pushTask(IterableOfSlot{.slot = &yield->value});
  this->state.walk.tasks.pushTask(CheckExprSlot{.slot = &yield->value, .expected = nullptr});
  if (!shape.isAsync) {
    this->state.walk.tasks.pushTask(MarkFused{.expr = yield->value});
  }
}

void YieldExpressions::startYieldValue(YieldExpr *yield, const YieldShape &shape) {
  if (yield->value == nullptr) {
    return this->walkEmptyYield(yield, shape);
  }
  this->state.walk.tasks.pushTask(AfterYieldValue{.yield = yield, .shape = shape});

  return this->state.walk.tasks.pushTask(
      CheckExprSlot{.slot = &yield->value, .expected = shape.yielded});
}

void YieldExpressions::afterYieldValue(const AfterYieldValue &task) {
  YieldExpr *yield = task.yield;
  const YieldShape &shape = task.shape;
  if (shape.isAsync) {
    return this->walkAsyncYieldValue(yield, shape);
  }
  if (shape.yielded != nullptr) {
    return this->state.walk.tasks.pushTask(
        CoerceSlot{.slot = &yield->value, .to = shape.yielded, .what = ""});
  }
}

void YieldExpressions::walkAsyncYieldValue(YieldExpr *yield, const YieldShape &shape) {
  const Type *type = yield->value->type;
  const Type *given = type->isError() ? type : AsyncChecker::awaitedType(type);
  auto fits = this->checkPromiseYield(type, given, shape);
  if (fits.isErr()) {
    return this->state.diagnostics.error(yield->value->loc(), fits.error().message);
  }
  if (shape.yielded != nullptr && !given->isError() && given == type) {
    return this->state.walk.tasks.pushTask(
        CoerceSlot{.slot = &yield->value, .to = shape.yielded, .what = ""});
  }
}

void YieldExpressions::afterYieldDelegate(const AfterYieldDelegate &task) {
  if (task.shape.isAsync) {
    return this->walkAsyncDelegate(task.yield, task.shape);
  }

  return this->walkSyncDelegate(task.yield, task.shape);
}

void YieldExpressions::finishYield(const FinishYield &task) {
  YieldExpr *yield = task.yield;
  const FuncSymbol *function = task.shape.function;
  if (!function->hasReturnType() && this->state.walk.functions.isCollectingYields()) {
    this->state.walk.functions.addCollectedYield(yield);
  }
  this->state.walk.tasks.pushChecked(yield);
  this->state.walk.tasks.pushTask(CalledFunction{});
  auto declared = this->checkSentDeclared(yield, task.shape);
  if (declared.isErr()) {
    return this->state.diagnostics.error(yield->loc(), declared.error().message);
  }
  yield->type = this->yieldResultType(yield, task.shape);
}

std::string YieldExpressions::shownYieldedName(const Type *yielded) const {
  if (yielded == nullptr) {
    return Type::intTy()->name().str();
  }

  return yielded->name().str();
}

YieldExpressions::ResultNothingSentAsync
YieldExpressions::checkAsyncSent(const Type *sent, const Type *yielded, bool isLoose) const {
  if (!sent->isVoid() && !isLoose) {
    auto shown = this->shownYieldedName(yielded);
    auto message = ErrorTextYield::asyncSent(shown);

    return err(CodeIteratorsRegistryAsyncSentError{
        .code = CodeIteratorsRegistryAsyncSentErrorCode::SENT_TO_ASYNC,
        .message = message,
    });
  }

  return ok(sent);
}

YieldExpressions::ResultPromiseYieldFits
YieldExpressions::checkPromiseYield(const Type *type, const Type *given,
                                    const YieldShape &shape) const {
  if (given == nullptr) {
    auto message = ErrorTextYield::mixed(type->name());

    return err(CodeIteratorsRegistryPromiseYieldError{
        .code = CodeIteratorsRegistryPromiseYieldErrorCode::MIXED_PROMISE,
        .message = message,
    });
  }
  bool isOther = shape.yielded != nullptr && !given->isError() && given != type &&
                 !this->state.types.relations.isImplicitlyConvertible(given, shape.yielded);
  if (isOther) {
    auto name = ErrorTextFunctionName::quoted(shape.function->name);
    auto message = ErrorTextYield::promise(given->name(), name, shape.yielded->name());

    return err(CodeIteratorsRegistryPromiseYieldError{
        .code = CodeIteratorsRegistryPromiseYieldErrorCode::OTHER_TYPE,
        .message = message,
    });
  }

  return ok(given);
}

YieldExpressions::ResultYieldGivesAValue
YieldExpressions::checkValueYielded(const YieldShape &shape) const {
  const Type *yielded = shape.yielded;
  if (yielded != nullptr && !yielded->isOptional() && !yielded->isError()) {
    auto name = ErrorTextFunctionName::quoted(shape.function->name);
    auto message = ErrorTextYield::noValue(name, yielded->name());

    return err(CodeIteratorsRegistryValueYieldedError{
        .code = CodeIteratorsRegistryValueYieldedErrorCode::UNDEFINED_YIELD,
        .message = message,
    });
  }

  return ok(yielded);
}

YieldExpressions::ResultDelegateAsyncFit
YieldExpressions::checkDelegateAsync(const Type *type, const Type *element,
                                     const YieldShape &shape) const {
  if (element == nullptr && !type->isError()) {
    auto message = ErrorTextYield::notAsync(type->name());

    return err(CodeIteratorsRegistryDelegateAsyncError{
        .code = CodeIteratorsRegistryDelegateAsyncErrorCode::NOT_ASYNC_ITERABLE,
        .message = message,
    });
  }
  bool isOther = element != nullptr && shape.yielded != nullptr &&
                 !this->state.types.relations.isImplicitlyConvertible(element, shape.yielded);
  if (isOther) {
    auto name = ErrorTextFunctionName::quoted(shape.function->name);
    auto message = ErrorTextYield::otherType(element->name(), name, shape.yielded->name());

    return err(CodeIteratorsRegistryDelegateAsyncError{
        .code = CodeIteratorsRegistryDelegateAsyncErrorCode::OTHER_TYPE,
        .message = message,
    });
  }

  return ok(element);
}

YieldExpressions::ResultDelegateSyncFits
YieldExpressions::checkDelegateSync(const Type *type, const Type *element,
                                    const YieldShape &shape) const {
  if (element == nullptr && !type->isError()) {
    auto message = ErrorTextYield::notIter(type->name());

    return err(CodeIteratorsRegistryDelegateSyncError{
        .code = CodeIteratorsRegistryDelegateSyncErrorCode::NOT_ITERABLE,
        .message = message,
    });
  }
  bool isSending = element != nullptr && type->isIterator() && !type->sent()->isVoid() &&
                   type->sent() != shape.sent && !shape.isLoose;
  if (isSending) {
    auto name = ErrorTextFunctionName::quoted(shape.function->name);
    std::string gets = shape.sent->isVoid() ? ErrorTextYield::nothing()
                                            : ErrorTextFunctionName::quoted(shape.sent->name());
    auto message = ErrorTextYield::sends(type->name(), type->sent()->name(), name, gets);

    return err(CodeIteratorsRegistryDelegateSyncError{
        .code = CodeIteratorsRegistryDelegateSyncErrorCode::SENDS_DIFFER,
        .message = message,
    });
  }
  bool isOther = element != nullptr && shape.yielded != nullptr &&
                 !this->state.types.relations.isImplicitlyConvertible(element, shape.yielded);
  if (isOther) {
    auto name = ErrorTextFunctionName::quoted(shape.function->name);
    auto message = ErrorTextYield::otherType(element->name(), name, shape.yielded->name());

    return err(CodeIteratorsRegistryDelegateSyncError{
        .code = CodeIteratorsRegistryDelegateSyncErrorCode::OTHER_TYPE,
        .message = message,
    });
  }

  return ok(element);
}

YieldExpressions::ResultSentTypeDeclared
YieldExpressions::checkSentDeclared(const YieldExpr *yield, const YieldShape &shape) const {
  bool isUntyped = !yield->delegate && !shape.isAsync && shape.sent->isVoid() &&
                   !yield->discarded && !shape.isLoose;
  if (isUntyped) {
    auto shown = this->shownYieldedName(shape.yielded);
    auto message = ErrorTextYield::untyped(shown);

    return err(CodeIteratorsRegistrySentDeclaredError{
        .code = CodeIteratorsRegistrySentDeclaredErrorCode::UNTYPED_SENT,
        .message = message,
    });
  }

  return ok(shape.sent);
}

YieldShape YieldExpressions::yieldShape(const YieldExpr *yield, FuncSymbol *function) {
  bool isAsync = function->decl->isAsync;
  const Type *known = function->hasReturnType() ? function->getKnownReturnType() : nullptr;
  bool isIterating = known != nullptr && this->isIteratorKind(known, isAsync);
  const Type *generator = isIterating ? known : nullptr;
  const Type *yielded = generator != nullptr ? generator->yielded() : nullptr;
  const Type *declaredSent = generator != nullptr ? generator->sent() : Type::voidTy();
  bool isLoose = function->hasReturnType() && generator == nullptr;
  const Type *sent =
      isAsync ? this->asyncSentType(yield, declaredSent, yielded, isLoose) : declaredSent;

  return YieldShape{
      .function = function,
      .isAsync = isAsync,
      .yielded = yielded,
      .sent = sent,
      .isLoose = isLoose,
  };
}

bool YieldExpressions::isIteratorKind(const Type *type, bool isAsync) const {
  if (isAsync) {
    return type->isAsyncIterator();
  }

  return type->isIterator();
}

const Type *YieldExpressions::asyncSentType(const YieldExpr *yield, const Type *sent,
                                            const Type *yielded, bool isLoose) {
  auto nothing = this->checkAsyncSent(sent, yielded, isLoose);
  if (nothing.isErr()) {
    this->state.diagnostics.error(yield->loc(), nothing.error().message);
    return Type::voidTy();
  }

  return sent;
}

void YieldExpressions::walkEmptyYield(const YieldExpr *yield, const YieldShape &shape) {
  auto value = this->checkValueYielded(shape);
  if (value.isErr()) {
    this->state.diagnostics.error(yield->loc(), value.error().message);
  }
}

void YieldExpressions::walkAsyncDelegate(YieldExpr *yield, const YieldShape &shape) {
  const Type *type = yield->value->type;
  const auto *element = this->state.code.iteration.asyncIteratedType(type);
  auto fits = this->checkDelegateAsync(type, element, shape);
  if (fits.isErr()) {
    this->state.diagnostics.error(yield->value->loc(), fits.error().message);
  }
  yield->element = element != nullptr ? element : Type::error();
}

void YieldExpressions::walkSyncDelegate(YieldExpr *yield, const YieldShape &shape) {
  const Type *type = yield->value->type;
  const auto *element = this->state.code.iteration.iteratedType(type);
  auto fits = this->checkDelegateSync(type, element, shape);
  if (fits.isErr()) {
    this->state.diagnostics.error(yield->value->loc(), fits.error().message);
  }
  yield->element = element != nullptr ? element : Type::error();
}

const Type *YieldExpressions::yieldResultType(const YieldExpr *yield,
                                              const YieldShape &shape) const {
  if (yield->delegate) {
    return this->delegateResultType(yield, shape);
  }
  if (shape.isAsync) {
    return Type::undefinedTy();
  }
  if (!shape.sent->isVoid()) {
    return shape.sent;
  }

  return Type::undefinedTy();
}

const Type *YieldExpressions::delegateResultType(const YieldExpr *yield,
                                                 const YieldShape &shape) const {
  const Type *type = yield->value->type;
  if (type->isError()) {
    return Type::error();
  }
  bool isReturned = !shape.isAsync && type->isIterator() && !type->returned()->isVoid();

  return isReturned ? type->returned() : Type::undefinedTy();
}

}
