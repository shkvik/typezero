#include "compiler/semantic/code/functions/array_callbacks/array_callbacks.h"
#include "compiler/program/library/array/array_method_table/array_method_table.h"
#include "compiler/program/library/builtin_names/global_name_table/global_name_table.h"
#include "compiler/semantic/code/calls/code_calls_registry_errors.h"
#include "compiler/semantic/code/functions/code_functions_registry_errors.h"
#include "compiler/semantic/flow/children_first/children_first.h"
#include "llvm/ADT/STLExtras.h"

using llvm::cast;
using llvm::dyn_cast;
using llvm::isa;

namespace tz {

ArrayCallbacks::ArrayCallbacks(SemanticState &owner)
    : state(owner) {}

void ArrayCallbacks::checkCallbackMethod(const CheckCallbackMethod &task) {
  CallExpr *call = task.call;
  MemberExpr *member = task.member;
  auto passed = this->checkCallbackGiven(call->args.size(), member->name);
  if (passed.isErr()) {
    this->state.diagnostics.error(member->loc(), passed.error().message);
    return this->state.walk.tasks.pushChecked(call);
  }
  bool isReduce = this->callbackKind(member) == ArrayCallback::REDUCE;
  this->state.walk.tasks.pushTask(AfterExtraCallbackArgs{
      .call = call,
      .member = member,
      .expected = task.expected,
  });

  return this->walkExtraCallbackArgs(call, member, isReduce);
}

ArrayCallback ArrayCallbacks::callbackKind(const MemberExpr *member) const {
  const auto *method = ArrayMethodTable::find(member->name);
  const CallbackArrayMethod *callbackMethod =
      method != nullptr ? method->form.as<CallbackArrayMethod>() : nullptr;

  return callbackMethod != nullptr ? callbackMethod->callback : ArrayCallback::COMPARE;
}

Builtin ArrayCallbacks::callbackBuiltin(const MemberExpr *member) const {
  const auto *method = ArrayMethodTable::find(member->name);
  const CallbackArrayMethod *callbackMethod =
      method != nullptr ? method->form.as<CallbackArrayMethod>() : nullptr;

  return callbackMethod != nullptr ? method->builtin : Builtin::ArrSortWith;
}

void ArrayCallbacks::walkExtraCallbackArgs(CallExpr *call, const MemberExpr *member,
                                           bool isReduce) {
  size_t count = call->args.size();
  size_t maxArgs = isReduce ? 2 : 1;
  auto extra = this->checkExtraArguments(count, maxArgs, member->name, isReduce);
  if (extra.isOk()) {
    return;
  }
  bool isThisArg =
      extra.error().code == CodeFunctionsRegistryExtraArgumentsErrorCode::THIS_ARGUMENT;
  llvm::SMLoc at = isThisArg ? call->args[1]->loc() : member->loc();
  this->state.diagnostics.error(at, extra.error().message);
  for (size_t index = count; index > maxArgs; --index) {
    this->state.walk.tasks.pushTask(
        CheckExprSlot{.slot = &call->args[index - 1], .expected = nullptr});
  }
}

void ArrayCallbacks::afterExtraCallbackArgs(const AfterExtraCallbackArgs &task) {
  CallExpr *call = task.call;
  MemberExpr *member = task.member;
  ArrayCallback kind = this->callbackKind(member);
  bool isReduce = kind == ArrayCallback::REDUCE;
  size_t maxArgs = isReduce ? 2 : 1;
  if (call->args.size() > maxArgs) {
    call->args.resize(maxArgs);
  }
  const Type *arrayType = member->object->type;
  bool isTyped = arrayType->isTypedArray();
  const Type *element = isTyped ? arrayType->elementType() : arrayType->inner();
  size_t count = call->args.size();
  bool isNotEmpty = this->state.flow.proofs.canProveNotEmpty(member->object);
  bool isBoolean = this->state.isGlobalName(call->args[0], GlobalName::BOOLEAN);
  auto shape = ArrayCallbackShape{
      .call = call,
      .member = member,
      .arrayType = arrayType,
      .element = element,
      .kind = kind,
      .isTyped = isTyped,
      .count = count,
      .accumulator = element,
      .hasSomeValue = !isReduce || count == 2 || isNotEmpty,
      .isBoolean = isBoolean,
  };
  if (isReduce && count == 2) {
    return this->startReduceInit(shape, task.expected);
  }

  return this->startCallbackArgument(shape, task.expected);
}

void ArrayCallbacks::startReduceInit(const ArrayCallbackShape &shape, const Type *expected) {
  Expr *init = shape.call->args[1];
  auto *unary = dyn_cast<UnaryExpr>(init);
  bool isNegative =
      unary != nullptr && unary->op == UnaryOp::Neg && isa<IntLitExpr>(unary->operand);
  bool isLiteral = isa<IntLitExpr>(init) || isNegative;
  const auto *elementType = this->state.generics.apparent.apparentType(shape.element);
  bool isDecimalStart =
      elementType->isDecimal() && this->state.code.conversions.isNumberLiteralTree(init);
  bool isWide = elementType->isInt() || elementType->isFloat();
  const Type *hint = isDecimalStart || (isLiteral && isWide) ? elementType : nullptr;
  this->state.walk.tasks.pushTask(AfterReduceInit{.shape = shape, .expected = expected});

  return this->state.walk.tasks.pushTask(
      CheckExprSlot{.slot = &shape.call->args[1], .expected = hint});
}

void ArrayCallbacks::afterReduceInit(const AfterReduceInit &task) {
  auto shape = task.shape;
  CallExpr *call = shape.call;
  const Type *accumulator = call->args[1]->type;
  auto initial = this->checkInitialValue(accumulator, shape.member->name);
  if (initial.isErr()) {
    this->state.diagnostics.error(call->args[1]->loc(), initial.error().message);
  }
  shape.accumulator = initial.isOk() ? accumulator : Type::error();

  return this->startCallbackArgument(shape, task.expected);
}

void ArrayCallbacks::startCallbackArgument(const ArrayCallbackShape &shape, const Type *expected) {
  CallExpr *call = shape.call;
  auto &tasks = this->state.walk.tasks;
  if (shape.accumulator->isError()) {
    tasks.pushChecked(call);
    return tasks.pushTask(CheckExprSlot{.slot = &call->args[0], .expected = nullptr});
  }
  auto signature = this->callbackSignature(shape, expected);
  this->rememberCallbackFacts(shape);
  bool isTwoPrimary = shape.kind == ArrayCallback::REDUCE || shape.kind == ArrayCallback::COMPARE;
  unsigned primary = isTwoPrimary ? 2 : 1;
  tasks.pushTask(FinishCallbackMethod{.shape = shape});

  return tasks.pushTask(CheckFunctionArgSlot{
      .slot = &call->args[0],
      .expected = std::move(signature),
      .primary = primary,
      .method = shape.member->name,
  });
}

void ArrayCallbacks::finishCallbackMethod(const FinishCallbackMethod &task) {
  const ArrayCallbackShape &shape = task.shape;
  CallExpr *call = shape.call;
  auto &tasks = this->state.walk.tasks;
  Builtin builtin = this->callbackBuiltin(shape.member);
  call->target = BuiltinCall{.builtin = builtin};
  tasks.pushChecked(call);
  if (call->args[0]->type->isFunction()) {
    tasks.pushTask(CheckArrayCallbackResult{.shape = shape});
  }

  return tasks.pushTask(CalledFunction{});
}

void ArrayCallbacks::checkArrayCallbackResult(const CheckArrayCallbackResult &task) {
  const ArrayCallbackShape &shape = task.shape;
  CallExpr *call = shape.call;
  const Type *result = call->args[0]->type->ret();
  switch (shape.kind) {
    case ArrayCallback::FOR_EACH:
      call->type = Type::voidTy();
      return;
    case ArrayCallback::MAP:
    case ArrayCallback::FLAT_MAP:
      return this->checkMappedResult(shape, result);
    case ArrayCallback::FILTER:
      return this->startFilterResult(shape);
    case ArrayCallback::REDUCE:
      return this->finishArrayReduce(shape);
    case ArrayCallback::FIND:
      call->type = Type::optional(shape.element);
      return;
    case ArrayCallback::FIND_INDEX:
      call->type = Type::intTy();
      return;
    case ArrayCallback::TEST:
      call->type = Type::boolTy();
      return;
    case ArrayCallback::COMPARE:
      return this->checkCompareResult(shape, result);
  }
}

void ArrayCallbacks::checkMappedResult(const ArrayCallbackShape &shape, const Type *result) {
  CallExpr *call = shape.call;
  auto value = this->checkMappedValue(result, shape.member->name);
  if (value.isErr()) {
    return this->state.diagnostics.error(call->args[0]->loc(), value.error().message);
  }
  if (shape.isTyped) {
    call->type = shape.arrayType;
    return;
  }
  bool isFlat = shape.kind == ArrayCallback::FLAT_MAP && result->isArray();
  call->type = isFlat ? result : Type::array(result);
}

void ArrayCallbacks::finishArrayReduce(const ArrayCallbackShape &shape) {
  CallExpr *call = shape.call;
  const Type *accumulator = shape.accumulator;
  call->type = shape.hasSomeValue ? accumulator : Type::optional(accumulator);
  call->mayBeOutOfRange = !shape.hasSomeValue;
  if (shape.count == 2 && call->args.size() == 2) {
    return this->state.walk.tasks.pushTask(
        CoerceSlot{.slot = &call->args[1], .to = accumulator, .what = ""});
  }
}

void ArrayCallbacks::checkCompareResult(const ArrayCallbackShape &shape, const Type *result) {
  CallExpr *call = shape.call;
  auto number = this->checkComparison(result);
  if (number.isErr()) {
    return this->state.diagnostics.error(call->args[0]->loc(), number.error().message);
  }
  call->type = shape.arrayType;
}

bool ArrayCallbacks::isKeepingDefined(const Expr *callback) const {
  const auto *returnStmt = this->singleReturn(callback);
  if (returnStmt == nullptr) {
    return false;
  }
  auto *functionExpr = cast<FunctionExpr>(callback);
  auto whole = SignedCondition{
      .cond = returnStmt->value,
      .isWantedTrue = true,
  };
  auto answers = std::vector<DefinedVars>();
  for (SignedCondition part : ChildrenFirst::order(whole, this, &ArrayCallbacks::signedParts)) {
    DefinedVars vars = this->definedBy(part, answers);
    answers.push_back(std::move(vars));
  }

  return answers.back().contains(functionExpr->fn->params[0].sym);
}

const ReturnStmt *ArrayCallbacks::singleReturn(const Expr *callback) const {
  auto *functionExpr = dyn_cast<FunctionExpr>(callback);
  bool isSingleStatement = functionExpr != nullptr && !functionExpr->fn->params.empty() &&
                           functionExpr->fn->params[0].sym != nullptr &&
                           functionExpr->fn->body->body.size() == 1;
  if (!isSingleStatement) {
    return nullptr;
  }
  auto *returnStmt = dyn_cast<ReturnStmt>(functionExpr->fn->body->body[0]);
  if (returnStmt == nullptr || returnStmt->value == nullptr) {
    return nullptr;
  }

  return returnStmt;
}

std::vector<const FuncSymbol *> ArrayCallbacks::definedNeeds(const Expr *callback) const {
  auto needs = std::vector<const FuncSymbol *>();
  const auto *returnStmt = this->singleReturn(callback);
  if (returnStmt == nullptr) {
    return needs;
  }
  auto whole = SignedCondition{
      .cond = returnStmt->value,
      .isWantedTrue = true,
  };
  for (SignedCondition part : ChildrenFirst::order(whole, this, &ArrayCallbacks::signedParts)) {
    const auto *cond = this->unconvertedCondition(part.cond);
    const auto *function = this->state.flow.functions.findPredicateNeed(cond);
    if (function != nullptr) {
      needs.push_back(function);
    }
  }

  return needs;
}

NodeChildren<SignedCondition> ArrayCallbacks::signedParts(SignedCondition part) const {
  auto *unary = dyn_cast<UnaryExpr>(part.cond);
  if (unary != nullptr && unary->op == UnaryOp::Not) {
    return {SignedCondition{unary->operand, !part.isWantedTrue}};
  }
  auto *binary = dyn_cast<BinaryExpr>(part.cond);
  bool isLogic =
      binary != nullptr && (binary->op == BinaryOp::LogAnd || binary->op == BinaryOp::LogOr);
  if (isLogic) {
    return {SignedCondition{binary->lhs, part.isWantedTrue},
            SignedCondition{binary->rhs, part.isWantedTrue}};
  }

  return {};
}

ArrayCallbacks::DefinedVars ArrayCallbacks::definedBy(SignedCondition part,
                                                      std::vector<DefinedVars> &answers) const {
  auto *unary = dyn_cast<UnaryExpr>(part.cond);
  if (unary != nullptr && unary->op == UnaryOp::Not) {
    return ChildrenFirst::takeAnswer(answers);
  }
  auto *binary = dyn_cast<BinaryExpr>(part.cond);
  bool isLogic =
      binary != nullptr && (binary->op == BinaryOp::LogAnd || binary->op == BinaryOp::LogOr);
  if (isLogic) {
    DefinedVars right = ChildrenFirst::takeAnswer(answers);
    DefinedVars left = ChildrenFirst::takeAnswer(answers);
    bool isAnd = binary->op == BinaryOp::LogAnd;
    return this->joinDefined(isAnd, part.isWantedTrue, left, right);
  }
  const auto *cond = this->unconvertedCondition(part.cond);
  auto whenTrue = Flow();
  auto whenFalse = Flow();
  this->state.flow.conditions.addConditionFacts(cond, whenTrue, whenFalse);

  return part.isWantedTrue ? whenTrue.defined : whenFalse.defined;
}

const Expr *ArrayCallbacks::unconvertedCondition(const Expr *cond) const {
  auto *conversion = dyn_cast<CastExpr>(cond);
  if (conversion != nullptr && conversion->isImplicit() && conversion->type->isBool()) {
    return conversion->operand;
  }

  return cond;
}

ArrayCallbacks::DefinedVars ArrayCallbacks::joinDefined(bool isAnd, bool isWantedTrue,
                                                        DefinedVars left,
                                                        const DefinedVars &right) const {
  if (isAnd == isWantedTrue) {
    left.insert(right.begin(), right.end());
    return left;
  }
  auto both = DefinedVars();
  for (const VarSymbol *variable : left) {
    if (right.contains(variable)) {
      both.insert(variable);
    }
  }

  return both;
}

ArrayCallbacks::ResultCallbackIsPassed
ArrayCallbacks::checkCallbackGiven(size_t count, llvm::StringRef name) const {
  if (count == 0) {
    auto message = ErrorTextCallbackMethod::noFunction(name);

    return err(CodeFunctionsRegistryCallbackGivenError{
        .code = CodeFunctionsRegistryCallbackGivenErrorCode::NO_FUNCTION,
        .message = message,
    });
  }

  return ok(count);
}

ArrayCallbacks::ResultNoExtraArguments ArrayCallbacks::checkExtraArguments(size_t count,
                                                                           size_t maxArgs,
                                                                           llvm::StringRef name,
                                                                           bool isReduce) const {
  if (count > maxArgs && count == 2 && !isReduce) {
    auto message = ErrorTextCallbackMethod::thisArg(name);

    return err(CodeFunctionsRegistryExtraArgumentsError{
        .code = CodeFunctionsRegistryExtraArgumentsErrorCode::THIS_ARGUMENT,
        .message = message,
    });
  }
  if (count > maxArgs) {
    auto message = ErrorTextArity::count(1, unsigned(maxArgs), count);

    return err(CodeFunctionsRegistryExtraArgumentsError{
        .code = CodeFunctionsRegistryExtraArgumentsErrorCode::TOO_MANY_ARGUMENTS,
        .message = message,
    });
  }

  return ok(count);
}

ArrayCallbacks::ResultInitialValueType
ArrayCallbacks::checkInitialValue(const Type *accumulator, llvm::StringRef name) const {
  if (accumulator->isUndefined() || accumulator->isVoid()) {
    auto message = ErrorTextCallbackMethod::untyped(name);

    return err(CodeFunctionsRegistryInitialValueError{
        .code = CodeFunctionsRegistryInitialValueErrorCode::UNTYPED_INITIAL,
        .message = message,
    });
  }

  return ok(accumulator);
}

ArrayCallbacks::ResultCallbackHasValue
ArrayCallbacks::checkMappedValue(const Type *result, llvm::StringRef name) const {
  if (result->isVoid() || result->isUndefined()) {
    auto message = ErrorTextCallbackMethod::noValue(name);

    return err(CodeFunctionsRegistryMappedValueError{
        .code = CodeFunctionsRegistryMappedValueErrorCode::NO_VALUE,
        .message = message,
    });
  }

  return ok(result);
}

ArrayCallbacks::ResultComparisonNumber ArrayCallbacks::checkComparison(const Type *result) const {
  if (!result->isNumeric() && !result->isDecimal()) {
    auto message = ErrorTextCallbackMethod::notNumber(result->name());

    return err(CodeFunctionsRegistryComparisonError{
        .code = CodeFunctionsRegistryComparisonErrorCode::NOT_A_NUMBER,
        .message = message,
    });
  }

  return ok(result);
}

Signature ArrayCallbacks::callbackSignature(const ArrayCallbackShape &shape,
                                            const Type *expected) const {
  const auto *ret = this->callbackReturn(shape, expected);
  auto params = this->callbackParams(shape);

  return Signature{
      .params = params,
      .ret = ret,
      .voidResult = shape.kind == ArrayCallback::FOR_EACH,
      .pad = false,
  };
}

const Type *ArrayCallbacks::callbackReturn(const ArrayCallbackShape &shape,
                                           const Type *expected) const {
  switch (shape.kind) {
    case ArrayCallback::MAP:
    case ArrayCallback::FLAT_MAP:
      return this->mappedReturn(shape, expected);
    case ArrayCallback::FILTER:
    case ArrayCallback::FIND:
    case ArrayCallback::FIND_INDEX:
    case ArrayCallback::TEST:
      return Type::boolTy();
    case ArrayCallback::REDUCE:
      return shape.accumulator;
    case ArrayCallback::FOR_EACH:
    case ArrayCallback::COMPARE:
      return nullptr;
  }

  return nullptr;
}

const Type *ArrayCallbacks::mappedReturn(const ArrayCallbackShape &shape,
                                         const Type *expected) const {
  if (shape.isTyped) {
    return shape.element;
  }
  bool isMap = shape.kind == ArrayCallback::MAP;
  const Type *wanted = expected != nullptr ? expected->nonOptional() : nullptr;
  bool isShaped = wanted != nullptr && wanted->isArray() &&
                  (wanted->inner()->holdsObjects() || wanted->inner()->holdsResults() ||
                   (isMap && wanted->inner()->nonOptional()->isArray()));
  if (!isShaped) {
    return nullptr;
  }

  return isMap ? wanted->inner() : wanted;
}

std::vector<const Type *> ArrayCallbacks::callbackParams(const ArrayCallbackShape &shape) const {
  if (shape.kind == ArrayCallback::COMPARE) {
    return {shape.element, shape.element};
  }
  if (shape.kind == ArrayCallback::REDUCE) {
    return {shape.accumulator, shape.element, Type::intTy(), shape.arrayType};
  }

  return {shape.element, Type::intTy(), shape.arrayType};
}

void ArrayCallbacks::rememberCallbackFacts(const ArrayCallbackShape &shape) {
  auto *functionExpr = dyn_cast<FunctionExpr>(shape.call->args[0]);
  if (functionExpr == nullptr) {
    return;
  }
  if (shape.kind != ArrayCallback::COMPARE) {
    this->rememberIndexFacts(shape, functionExpr->fn);
  }
  if (shape.isTyped) {
    this->rememberByteFacts(shape, functionExpr->fn);
  }
}

void ArrayCallbacks::rememberIndexFacts(const ArrayCallbackShape &shape, FunctionDecl *callback) {
  bool isReduce = shape.kind == ArrayCallback::REDUCE;
  unsigned indexParam = isReduce ? 2 : 1;
  this->state.flow.callbacks.rememberIndexCallback(callback, indexParam);
  auto *literal = dyn_cast<ArrayLitExpr>(shape.member->object);
  if (literal == nullptr || !shape.element->isInt()) {
    return;
  }
  unsigned elementParam = isReduce ? 1 : 0;
  auto elements = this->literalElements(literal, elementParam);
  if (elements.has_value()) {
    this->state.flow.callbacks.rememberElementCallback(callback, *elements);
  }
}

void ArrayCallbacks::rememberByteFacts(const ArrayCallbackShape &shape, FunctionDecl *callback) {
  int64_t low = 0;
  int64_t high = 0;
  shape.arrayType->elementRange(low, high);
  bool isMap = shape.kind == ArrayCallback::MAP;
  auto info = ByteCallback{
      .param = shape.kind == ArrayCallback::REDUCE ? 1u : 0u,
      .lo = low,
      .hi = high,
      .target = isMap ? shape.arrayType : nullptr,
  };
  this->state.flow.callbacks.rememberByteCallback(callback, info);
}

std::optional<ElementCallback> ArrayCallbacks::literalElements(const ArrayLitExpr *literal,
                                                               unsigned param) {
  std::optional<int64_t> low;
  std::optional<int64_t> high;
  for (const ArrayLitExpr::Element &element : literal->elements) {
    std::optional<int64_t> value = element.spread || element.hole
                                       ? std::nullopt
                                       : this->state.code.constants.intConstant(element.expr);
    if (!value.has_value()) {
      return std::nullopt;
    }
    low = low.has_value() ? std::min(*low, *value) : *value;
    high = high.has_value() ? std::max(*high, *value) : *value;
  }
  if (!low.has_value()) {
    return std::nullopt;
  }

  return ElementCallback{
      .param = param,
      .lo = *low,
      .hi = *high,
  };
}

void ArrayCallbacks::startFilterResult(const ArrayCallbackShape &shape) {
  const auto *function = this->state.flow.functions.findFilterNeed(shape.call->args[0]);
  if (function == nullptr) {
    return this->finishFilterResult(FinishFilterResult{.shape = shape});
  }
  auto &tasks = this->state.walk.tasks;
  tasks.pushTask(FinishFilterResult{.shape = shape});

  return tasks.pushTask(BuildPredicate{.function = function});
}

void ArrayCallbacks::finishFilterResult(const FinishFilterResult &task) {
  const ArrayCallbackShape &shape = task.shape;
  const Expr *predicate = shape.call->args[0];
  const Type *element = shape.element;
  const auto *kept = this->state.flow.functions.filteredType(predicate, element);
  if (kept != nullptr) {
    shape.call->type = Type::array(kept);
    return;
  }
  if (!element->isOptional() || shape.isBoolean) {
    shape.call->type = element->isOptional() ? Type::array(element->inner()) : shape.arrayType;
    return;
  }
  auto &tasks = this->state.walk.tasks;
  tasks.pushTask(FinishFilterDefined{.shape = shape});
  for (const FuncSymbol *function : llvm::reverse(this->definedNeeds(predicate))) {
    tasks.pushTask(BuildPredicate{.function = function});
  }
}

void ArrayCallbacks::finishFilterDefined(const FinishFilterDefined &task) {
  const ArrayCallbackShape &shape = task.shape;
  bool isDefined = this->isKeepingDefined(shape.call->args[0]);
  shape.call->type = isDefined ? Type::array(shape.element->inner()) : shape.arrayType;
}

}
