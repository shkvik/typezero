#include "compiler/semantic/code/calls/call_arguments/call_arguments.h"
#include "llvm/ADT/STLExtras.h"

namespace tz {

namespace {

constexpr unsigned kAnyCount = ~0u;

}

CallArguments::CallArguments(SemanticState &owner, CallRules &sharedCallRules)
    : callRules(sharedCallRules),
      state(owner) {}

void CallArguments::afterCalleeValue(const AfterCalleeValue &task) {
  CallExpr *call = task.call;
  if (call->optional) {
    call->callee = this->callRules.chainLink(call->callee);
  }
  auto callable = this->checkCalleeCallable(call->callee->type, task.calleeName);
  if (callable.isOk()) {
    return this->checkValueCall(CheckValueCall{.call = call, .type = callable.value()});
  }
  if (callable.error().code != CodeCallsRegistryCalleeCallableErrorCode::ALREADY_REPORTED) {
    this->state.diagnostics.error(call->callee->loc(), callable.error().message);
  }
  this->state.walk.tasks.pushChecked(call);

  return this->state.walk.tasks.pushTask(WalkArgsLoosely{.call = call});
}

void CallArguments::checkValueCall(const CheckValueCall &task) {
  CallExpr *call = task.call;
  const Type *type = task.type;
  call->type = type->ret();
  if (type->hasThis()) {
    auto message = ErrorTextValueCall::takesThis();
    this->state.walk.tasks.pushChecked(call);
    this->state.walk.tasks.pushTask(ReportError{.loc = call->callee->loc(), .message = message});
    return this->state.walk.tasks.pushTask(WalkArgsLoosely{.call = call});
  }
  llvm::ArrayRef<const Type *> params = type->params();

  return this->checkArguments(CheckArguments{
      .call = call,
      .params = std::vector<const Type *>(params.begin(), params.end()),
      .minArgs = type->minArgs(),
      .hasRest = type->hasRest(),
      .loc = call->callee->loc(),
      .checked = {},
  });
}

void CallArguments::afterDirectReturnType(const AfterDirectReturnType &task) {
  FuncSymbol *function = task.function;
  task.call->type = this->state.walk.tasks.takeAnswer();

  return this->checkArguments(CheckArguments{
      .call = task.call,
      .params = function->params,
      .minArgs = function->minArgs,
      .hasRest = function->hasRest,
      .loc = task.loc,
      .checked = {},
  });
}

bool CallArguments::rejectsSpreadBefore(CallExpr *call, size_t limit) {
  for (size_t index = 0; index < call->args.size() && index < limit; ++index) {
    if (call->spreads(index) && this->callRules.rejectSpread(call)) {
      return true;
    }
  }

  return false;
}

void CallArguments::checkArguments(const CheckArguments &task) {
  CallExpr *call = task.call;
  if (task.checked.empty()) {
    this->callRules.expandListSpreads(call->args, call->spread);
  }
  auto shape = ArgumentShape{
      .params = task.params,
      .fixed = task.params.size() - (task.hasRest ? 1 : 0),
      .loc = task.loc,
      .checked = task.checked,
  };
  auto least = static_cast<unsigned>(task.minArgs);
  bool isListParam = task.hasRest && (task.params.back()->isListParam() ||
                                      this->callRules.isSpreadingLists(task.params.back()));
  if (isListParam) {
    return this->startListParamArguments(call, shape, least);
  }
  size_t spreadLimit = task.hasRest ? shape.fixed : call->args.size();
  if (this->rejectsSpreadBefore(call, spreadLimit)) {
    return;
  }
  unsigned most = task.hasRest ? kAnyCount : static_cast<unsigned>(task.params.size());
  auto arity = this->checkArity(call, least, most);
  if (arity.isErr()) {
    this->state.walk.tasks.pushChecked(call);
    this->state.walk.tasks.pushTask(ReportError{.loc = task.loc, .message = arity.error().message});
    return this->state.walk.tasks.pushTask(WalkArgsLoosely{.call = call});
  }
  this->state.walk.tasks.pushChecked(call);
  this->state.walk.tasks.pushTask(CalledFunction{});
  if (!task.hasRest) {
    return this->pushTypedArguments(call, shape, call->args.size());
  }
  return this->startRestArguments(call, shape);
}

void CallArguments::startRestArguments(CallExpr *call, const ArgumentShape &shape) {
  const Type *last = shape.params.back();
  const Type *listRest = last->isTuple() || last->isListParam() ? last : nullptr;
  size_t fixed = shape.fixed;
  bool isSpreadOnly = listRest != nullptr && call->args.size() == fixed + 1 && call->spreads(fixed);
  if (isSpreadOnly) {
    this->pushTypedArguments(call, shape, fixed);
    this->state.walk.tasks.pushTask(
        AfterSpreadRest{.call = call, .fixed = fixed, .listRest = listRest});
    if (!this->isChecked(shape, fixed)) {
      this->state.walk.tasks.pushTask(
          CheckExprSlot{.slot = &call->args[fixed], .expected = listRest});
    }
    return;
  }
  auto coercions = std::vector<std::pair<Expr **, const Type *>>();
  bool hasChecked = this->packRestElements(call, shape, listRest, coercions);
  this->pushTypedArguments(call, shape, hasChecked ? fixed : fixed + 1);
  for (const auto &[slot, to] : llvm::reverse(coercions)) {
    this->state.walk.tasks.pushTask(CoerceSlot{.slot = slot, .to = to, .what = ""});
  }
}

void CallArguments::afterSpreadRest(const AfterSpreadRest &task) {
  CallExpr *call = task.call;
  Expr *arg = call->args[task.fixed];
  auto fits = this->checkRestType(arg, task.listRest);
  if (fits.isErr()) {
    this->state.diagnostics.error(arg->loc(), fits.error().message);
  }
  call->spread.clear();
}

bool CallArguments::packRestElements(CallExpr *call, const ArgumentShape &shape,
                                     const Type *listRest,
                                     std::vector<std::pair<Expr **, const Type *>> &coercions) {
  size_t fixed = shape.fixed;
  llvm::SMLoc restLoc = call->args.size() > fixed ? call->args[fixed]->loc() : call->loc();
  auto *rest = this->state.ast.make<ArrayLitExpr>(restLoc);
  bool hasChecked = false;
  for (size_t index = fixed; index < call->args.size(); ++index) {
    bool isChecked = this->isChecked(shape, index);
    hasChecked = hasChecked || isChecked;
    rest->elements.push_back(
        ArrayLitExpr::Element{.expr = call->args[index], .spread = call->spreads(index)});
  }
  for (size_t index = fixed; index < call->args.size(); ++index) {
    if (!this->isChecked(shape, index)) {
      continue;
    }
    Expr **slot = &rest->elements[index - fixed].expr;
    if (!call->spreads(index)) {
      coercions.emplace_back(slot, this->restElementType(index, shape, listRest));
      continue;
    }
    auto fits = this->checkRestType(*slot, shape.params.back());
    if (fits.isErr()) {
      this->state.diagnostics.error((*slot)->loc(), fits.error().message);
    }
  }
  auto counted = this->checkTupleRestCount(rest, listRest, fixed, call->args.size());
  if (hasChecked && counted.isErr()) {
    this->state.diagnostics.error(rest->loc(), counted.error().message);
    rest->type = Type::error();
  } else if (hasChecked) {
    rest->type = shape.params.back();
  }
  this->replaceRestArgs(call, fixed, rest);

  return hasChecked;
}

void CallArguments::pushTypedArguments(CallExpr *call, const ArgumentShape &shape, size_t count) {
  for (size_t index = count; index > 0; --index) {
    Expr **slot = &call->args[index - 1];
    const Type *to = shape.params[index - 1];
    if (this->isChecked(shape, index - 1)) {
      this->state.walk.tasks.pushTask(CoerceSlot{.slot = slot, .to = to, .what = ""});
    } else {
      this->state.walk.tasks.pushTask(CheckArgSlot{.slot = slot, .to = to});
    }
  }
}

void CallArguments::startListParamArguments(CallExpr *call, const ArgumentShape &shape,
                                            unsigned least) {
  if (this->rejectsSpreadBefore(call, shape.fixed)) {
    return;
  }
  auto arity = this->checkArity(call, least, kAnyCount);
  this->state.walk.tasks.pushChecked(call);
  if (arity.isErr()) {
    this->state.walk.tasks.pushTask(
        ReportError{.loc = shape.loc, .message = arity.error().message});
    return this->state.walk.tasks.pushTask(WalkArgsLoosely{.call = call});
  }
  this->state.walk.tasks.pushTask(CheckListArguments{
      .call = call,
      .fixed = shape.fixed,
      .listParam = shape.params.back(),
      .loc = shape.loc,
      .checked = shape.checked,
  });

  return this->pushTypedArguments(call, shape, shape.fixed);
}

CallArguments::ResultArityMatches CallArguments::checkArity(const CallExpr *call, unsigned min,
                                                            unsigned max) const {
  size_t count = call->args.size();
  bool isFitting = count >= min && count <= max;
  if (!isFitting && call->tagged) {
    auto message = ErrorTextArity::tagged(min, max, count);

    return err(CodeCallsRegistryCheckArityError{
        .code = CodeCallsRegistryCheckArityErrorCode::WRONG_COUNT_TAGGED,
        .message = message,
    });
  }
  if (!isFitting) {
    auto message = ErrorTextArity::count(min, max, count);

    return err(CodeCallsRegistryCheckArityError{
        .code = CodeCallsRegistryCheckArityErrorCode::WRONG_COUNT,
        .message = message,
    });
  }

  return ok(call);
}

void CallArguments::walkArgsLoosely(const WalkArgsLoosely &task) {
  for (Expr *&arg : llvm::reverse(task.call->args)) {
    this->state.walk.tasks.pushTask(CheckExprSlot{.slot = &arg, .expected = nullptr});
  }
}

void CallArguments::checkArgSlot(const CheckArgSlot &task) {
  const Type *expected = task.to->isError() ? nullptr : task.to;
  this->state.walk.tasks.pushTask(CoerceSlot{.slot = task.slot, .to = task.to, .what = ""});

  return this->state.walk.tasks.pushTask(CheckExprSlot{.slot = task.slot, .expected = expected});
}

CallArguments::ResultCalleeCallable
CallArguments::checkCalleeCallable(const Type *type, const NameExpr *calleeName) const {
  bool isOptional = type->isOptional() && type->inner()->isFunction();
  if (isOptional) {
    auto message = ErrorTextCallee::undefined();

    return err(CodeCallsRegistryCalleeCallableError{
        .code = CodeCallsRegistryCalleeCallableErrorCode::MAY_BE_UNDEFINED,
        .message = message,
    });
  }
  if (type->isError()) {
    return err(CodeCallsRegistryCalleeCallableError{
        .code = CodeCallsRegistryCalleeCallableErrorCode::ALREADY_REPORTED,
        .message = "",
    });
  }
  if (!type->isFunction() && calleeName != nullptr) {
    auto message = ErrorTextCallee::notFunc(calleeName->name, type->name());

    return err(CodeCallsRegistryCalleeCallableError{
        .code = CodeCallsRegistryCalleeCallableErrorCode::NOT_A_FUNCTION,
        .message = message,
    });
  }
  if (!type->isFunction()) {
    auto message = ErrorTextCallee::uncallable(type->name());

    return err(CodeCallsRegistryCalleeCallableError{
        .code = CodeCallsRegistryCalleeCallableErrorCode::NOT_CALLABLE,
        .message = message,
    });
  }

  return ok(type);
}

CallArguments::ResultSpreadRestFits CallArguments::checkRestType(const Expr *arg,
                                                                 const Type *to) const {
  if (arg->type != to && !arg->type->isError() && !to->isError()) {
    auto message = ErrorTextSpreadRest::otherType(arg->type->name(), to->name());

    return err(CodeCallsRegistrySpreadRestError{
        .code = CodeCallsRegistrySpreadRestErrorCode::OTHER_TYPE,
        .message = message,
    });
  }

  return ok(arg);
}

CallArguments::ResultTupleRestCount CallArguments::checkTupleRestCount(const ArrayLitExpr *rest,
                                                                       const Type *listRest,
                                                                       size_t fixed,
                                                                       size_t count) const {
  bool isTuple = listRest != nullptr && listRest->isTuple();
  if (isTuple && rest->elements.size() != listRest->fields().size()) {
    auto message = ErrorTextTupleRest::count(fixed + listRest->fields().size(), count);

    return err(CodeCallsRegistryTupleRestError{
        .code = CodeCallsRegistryTupleRestErrorCode::WRONG_COUNT,
        .message = message,
    });
  }

  return ok(rest);
}

bool CallArguments::isChecked(const ArgumentShape &shape, size_t index) const {
  return index < shape.checked.size() && shape.checked[index];
}

void CallArguments::replaceRestArgs(CallExpr *call, size_t fixed, ArrayLitExpr *rest) {
  call->args.resize(std::min(call->args.size(), fixed));
  while (call->args.size() < fixed) {
    call->args.push_back(this->state.ast.make<UndefinedExpr>(call->loc()));
  }
  call->args.push_back(rest);
  call->spread.clear();
}

const Type *CallArguments::restElementType(size_t index, const ArgumentShape &shape,
                                           const Type *listRest) const {
  const Type *restType = shape.params.back();
  size_t at = index - shape.fixed;
  bool isTupleSlot = listRest != nullptr && listRest->isTuple() && at < listRest->fields().size();
  if (isTupleSlot) {
    return listRest->fields()[at].type;
  }

  return restType->isArray() ? restType->inner() : restType;
}

}
