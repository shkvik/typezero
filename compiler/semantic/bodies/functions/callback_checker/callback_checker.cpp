#include "compiler/semantic/bodies/functions/callback_checker/callback_checker.h"
#include "compiler/program/library/array/array_method_table/array_method_table.h"
#include "compiler/program/library/builtin_names/global_name_table/global_name_table.h"
#include "compiler/semantic/bodies/expr/argument_checker/argument_checker_errors.h"
#include "compiler/semantic/bodies/expr/call_checker/call_checker.h"
#include "compiler/semantic/bodies/expr/expr_checker/expr_checker.h"
#include "compiler/semantic/bodies/functions/closure_checker/closure_checker.h"
#include "compiler/semantic/bodies/functions/function_checker/function_checker_errors.h"
#include "compiler/semantic/bodies/functions/function_value_checker/function_value_checker.h"
#include "compiler/semantic/bodies/functions/nested_functions/nested_functions.h"
#include "compiler/semantic/declare/constant_folder/constant_folder.h"
#include "compiler/semantic/flow/children_first/children_first.h"
#include "compiler/semantic/flow/condition_facts/condition_facts.h"
#include "compiler/semantic/flow/flow_checker/flow_checker.h"
#include "compiler/semantic/flow/index_proofs/index_proofs.h"
#include "compiler/semantic/flow/predicate_checker/predicate_checker.h"
#include "compiler/semantic/generics/generic_checker/generic_checker.h"
#include "compiler/semantic/generics/generic_instantiator/generic_instantiator.h"
#include "compiler/semantic/library/decimal/decimal_checker/decimal_checker.h"
#include "compiler/semantic/semantic_parts/semantic_parts.h"
#include "compiler/semantic/types/type_relations/type_relations.h"
#include "llvm/ADT/STLExtras.h"

using llvm::dyn_cast;
using llvm::isa;

namespace tz {

CallbackChecker::CallbackChecker(SemanticState &owner, SemanticParts &all)
    : state(owner) {
  this->callChecker = &all.callChecker;
  this->exprChecker = &all.exprChecker;
  this->flowChecker = &all.flowChecker;
  this->indexProofs = &all.indexProofs;
  this->typeRelations = &all.typeRelations;
  this->closureChecker = &all.closureChecker;
  this->conditionFacts = &all.conditionFacts;
  this->constantFolder = &all.constantFolder;
  this->genericChecker = &all.genericChecker;
  this->decimalChecker = &all.decimalChecker;
  this->nestedFunctions = &all.nestedFunctions;
  this->predicateChecker = &all.predicateChecker;
  this->genericInstantiator = &all.genericInstantiator;
  this->functionValueChecker = &all.functionValueChecker;
}

CallbackChecker::ResultFunctionIsPassed
CallbackChecker::checkFunctionGiven(const Type *type, llvm::StringRef method) const {
  if (!type->isFunction()) {
    auto message = ErrorTextCallbackArg::notFunc(method, type->name());

    return err(CallbackCheckerFunctionGivenError{
        .code = CallbackCheckerFunctionGivenErrorCode::NOT_A_FUNCTION,
        .message = message,
    });
  }

  return ok(type);
}

CallbackChecker::ResultWithoutRestParam
CallbackChecker::checkFixedParams(const Type *type, llvm::StringRef name) const {
  if (type->hasRest()) {
    auto message = ErrorTextCallbackArg::rest(name);

    return err(CallbackCheckerFixedParamsError{
        .code = CallbackCheckerFixedParamsErrorCode::REST_PARAMETER,
        .message = message,
    });
  }

  return ok(type);
}

CallbackChecker::ResultParameterMatches
CallbackChecker::checkCallbackParam(const Type *type, const Signature &expected, size_t index,
                                    unsigned primary, const FunctionDecl *known,
                                    llvm::StringRef name, llvm::StringRef method) const {
  const Type *param = type->params()[index];
  const Type *given = expected.params[index];
  bool isFitting = param == given || (param->isOptional() && param->inner() == given);
  if (!isFitting) {
    auto position = ErrorTextCallbackArg::position(index);
    auto message =
        ErrorTextCallbackArg::paramType(name, param->name(), position, method, given->name());

    return err(CallbackCheckerCallbackParamError{
        .code = CallbackCheckerCallbackParamErrorCode::PARAM_TYPE,
        .message = message,
    });
  }
  if (index >= primary && index >= type->minArgs()) {
    bool isNamed = known != nullptr && index < known->params.size();
    std::string paramName =
        isNamed ? ErrorTextCallbackArg::paramName(known->params[index].name) : "";
    auto argName = ErrorTextCallbackArg::argName(method, index);
    auto args = ErrorTextCallbackArg::argsShape(method);
    auto message = ErrorTextCallbackArg::extraParam(name, argName, paramName, method, args);

    return err(CallbackCheckerCallbackParamError{
        .code = CallbackCheckerCallbackParamErrorCode::EXTRA_OPTIONAL,
        .message = message,
    });
  }

  return ok(param);
}

CallbackChecker::ResultArgumentsSuffice
CallbackChecker::checkArgumentsEnough(const Type *type, const Signature &expected,
                                      llvm::StringRef name, llvm::StringRef method) const {
  if (type->minArgs() > expected.params.size()) {
    unsigned count = unsigned(expected.params.size());
    auto passes = ErrorTextArity::wanted(count, count);
    std::string needs = std::to_string(type->minArgs());
    auto message = ErrorTextCallbackArg::tooFew(method, passes, name, needs);

    return err(CallbackCheckerArgumentsEnoughError{
        .code = CallbackCheckerArgumentsEnoughErrorCode::TOO_FEW_PASSED,
        .message = message,
    });
  }

  return ok(type);
}

CallbackChecker::ResultReturnedTypeFits
CallbackChecker::checkReturnMatches(const Type *type, const Signature &expected,
                                    llvm::StringRef name, llvm::StringRef method) const {
  if (expected.ret != nullptr && type->ret() != expected.ret && !expected.voidResult) {
    auto message =
        ErrorTextCallbackArg::badReturn(method, expected.ret->name(), name, type->ret()->name());

    return err(CallbackCheckerReturnMatchesError{
        .code = CallbackCheckerReturnMatchesErrorCode::WRONG_RETURN,
        .message = message,
    });
  }

  return ok(type);
}

CallbackChecker::ResultNothingIsDropped
CallbackChecker::checkResultKept(const Type *type, const Signature &expected, llvm::StringRef name,
                                 llvm::StringRef method) const {
  if (expected.voidResult && type->ret()->isResult()) {
    auto message = ErrorTextCallbackArg::noCheck(name, method);

    return err(CallbackCheckerResultKeptError{
        .code = CallbackCheckerResultKeptErrorCode::RESULT_DROPPED,
        .message = message,
    });
  }
  if (expected.voidResult && !expected.asyncVoid && type->ret()->isPromise()) {
    auto message = ErrorTextCallbackArg::noWait(name, method);

    return err(CallbackCheckerResultKeptError{
        .code = CallbackCheckerResultKeptErrorCode::PROMISE_DROPPED,
        .message = message,
    });
  }

  return ok(type);
}

Expr *CallbackChecker::checkFunctionArg(Expr *arg, const Signature &expected, unsigned primary,
                                        llvm::StringRef method) {
  auto *functionExpr = dyn_cast<FunctionExpr>(arg);
  if (functionExpr != nullptr) {
    return this->checkLiteralCallback(functionExpr, expected, method);
  }
  auto *wrapper = this->functionValueChecker->builtinFunctionValue(arg, expected, primary, method);
  if (wrapper != nullptr) {
    wrapper->fn->nonEscaping = true;
    return this->closureChecker->checkFunctionExpr(wrapper, &expected);
  }
  auto *value = this->checkCallbackValue(arg, expected);

  return this->checkPassedFunction(value, expected, primary, method);
}

Expr *CallbackChecker::checkLiteralCallback(FunctionExpr *functionExpr, const Signature &expected,
                                            llvm::StringRef method) {
  functionExpr->fn->nonEscaping = true;
  auto *checked = this->closureChecker->checkFunctionExpr(functionExpr, &expected);
  auto awaited = this->functionValueChecker->checkAsyncCallback(checked, expected, method);
  if (awaited.isErr()) {
    this->state.getDiagnostics().error(checked->loc(), awaited.error().message);
    checked->type = Type::error();
  }

  return checked;
}

Expr *CallbackChecker::checkCallbackValue(Expr *arg, const Signature &expected) {
  const Signature *memberSignature = isa<MemberExpr>(arg) ? &expected : nullptr;
  this->state.getFunctionStack().enterCallback(memberSignature);
  auto *named = dyn_cast<NameExpr>(arg);
  FuncSymbol *generic = named != nullptr ? this->genericChecker->genericNamed(named) : nullptr;
  Expr *checked = generic != nullptr
                      ? this->genericInstantiator->checkGenericValue(named, generic, &expected)
                      : this->exprChecker->checkExpr(arg);
  this->state.getFunctionStack().leaveCallback();

  return checked;
}

Expr *CallbackChecker::checkPassedFunction(Expr *arg, const Signature &expected, unsigned primary,
                                           llvm::StringRef method) {
  const Type *type = arg->type;
  if (type->isError()) {
    return arg;
  }
  auto given = this->checkFunctionGiven(type, method);
  if (given.isErr()) {
    this->state.getDiagnostics().error(arg->loc(), given.error().message);
    return arg;
  }
  auto *known = this->nestedFunctions->namedFunction(arg);
  llvm::StringRef knownName = known != nullptr ? known->name : llvm::StringRef();
  auto name = ErrorTextFunctionName::quoted(knownName);
  auto fixed = this->checkFixedParams(type, name);
  if (fixed.isErr()) {
    this->state.getDiagnostics().error(arg->loc(), fixed.error().message);
    return arg;
  }
  if (this->reportCallbackParams(arg, expected, primary, known, name, method)) {
    return arg;
  }
  auto enough = this->checkArgumentsEnough(type, expected, name, method);
  if (enough.isErr()) {
    this->state.getDiagnostics().error(arg->loc(), enough.error().message);
    return arg;
  }

  return this->checkCallbackResult(arg, expected, name, method);
}

bool CallbackChecker::reportCallbackParams(const Expr *arg, const Signature &expected,
                                           unsigned primary, const FunctionDecl *known,
                                           llvm::StringRef name, llvm::StringRef method) {
  const Type *type = arg->type;
  size_t passed = std::min(type->params().size(), expected.params.size());
  for (size_t index = 0; index < passed; ++index) {
    auto param = this->checkCallbackParam(type, expected, index, primary, known, name, method);
    if (param.isErr()) {
      this->state.getDiagnostics().error(arg->loc(), param.error().message);
      return true;
    }
  }

  return false;
}

Expr *CallbackChecker::checkCallbackResult(Expr *arg, const Signature &expected,
                                           llvm::StringRef name, llvm::StringRef method) {
  const Type *type = arg->type;
  auto awaited = this->functionValueChecker->checkAsyncCallback(arg, expected, method);
  if (awaited.isErr()) {
    this->state.getDiagnostics().error(arg->loc(), awaited.error().message);
    arg->type = Type::error();
    return arg;
  }
  auto returns = this->checkReturnMatches(type, expected, name, method);
  if (returns.isErr()) {
    this->state.getDiagnostics().error(arg->loc(), returns.error().message);
    return arg;
  }
  auto kept = this->checkResultKept(type, expected, name, method);
  if (kept.isErr()) {
    this->state.getDiagnostics().error(arg->loc(), kept.error().message);
  }

  return arg;
}

bool CallbackChecker::isKeepingDefined(const Expr *callback) const {
  auto *functionExpr = dyn_cast<FunctionExpr>(callback);
  bool isSingleStatement = functionExpr != nullptr && !functionExpr->fn->params.empty() &&
                           functionExpr->fn->params[0].sym != nullptr &&
                           functionExpr->fn->body->body.size() == 1;
  if (!isSingleStatement) {
    return false;
  }
  auto *returnStmt = dyn_cast<ReturnStmt>(functionExpr->fn->body->body[0]);
  if (returnStmt == nullptr || returnStmt->value == nullptr) {
    return false;
  }
  auto whole = SignedCondition{
      .cond = returnStmt->value,
      .isWantedTrue = true,
  };
  auto answers = std::vector<DefinedVars>();
  for (SignedCondition part : ChildrenFirst::order(whole, this, &CallbackChecker::signedParts)) {
    DefinedVars vars = this->definedBy(part, answers);
    answers.push_back(std::move(vars));
  }

  return answers.back().contains(functionExpr->fn->params[0].sym);
}

NodeChildren<SignedCondition> CallbackChecker::signedParts(SignedCondition part) const {
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

CallbackChecker::DefinedVars CallbackChecker::definedBy(SignedCondition part,
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
  this->conditionFacts->addConditionFacts(cond, whenTrue, whenFalse);

  return part.isWantedTrue ? whenTrue.defined : whenFalse.defined;
}

const Expr *CallbackChecker::unconvertedCondition(const Expr *cond) const {
  auto *conversion = dyn_cast<CastExpr>(cond);
  if (conversion != nullptr && conversion->isImplicit() && conversion->type->isBool()) {
    return conversion->operand;
  }

  return cond;
}

CallbackChecker::DefinedVars CallbackChecker::joinDefined(bool isAnd, bool isWantedTrue,
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

CallbackChecker::ResultCallbackIsPassed
CallbackChecker::checkCallbackGiven(size_t count, llvm::StringRef name) const {
  if (count == 0) {
    auto message = ErrorTextCallbackMethod::noFunction(name);

    return err(CallbackCheckerCallbackGivenError{
        .code = CallbackCheckerCallbackGivenErrorCode::NO_FUNCTION,
        .message = message,
    });
  }

  return ok(count);
}

CallbackChecker::ResultNoExtraArguments CallbackChecker::checkExtraArguments(size_t count,
                                                                             size_t maxArgs,
                                                                             llvm::StringRef name,
                                                                             bool isReduce) const {
  if (count > maxArgs && count == 2 && !isReduce) {
    auto message = ErrorTextCallbackMethod::thisArg(name);

    return err(CallbackCheckerExtraArgumentsError{
        .code = CallbackCheckerExtraArgumentsErrorCode::THIS_ARGUMENT,
        .message = message,
    });
  }
  if (count > maxArgs) {
    auto message = ErrorTextArity::count(1, unsigned(maxArgs), count);

    return err(CallbackCheckerExtraArgumentsError{
        .code = CallbackCheckerExtraArgumentsErrorCode::TOO_MANY_ARGUMENTS,
        .message = message,
    });
  }

  return ok(count);
}

CallbackChecker::ResultInitialValueType
CallbackChecker::checkInitialValue(const Type *accumulator, llvm::StringRef name) const {
  if (accumulator->isUndefined() || accumulator->isVoid()) {
    auto message = ErrorTextCallbackMethod::untyped(name);

    return err(CallbackCheckerInitialValueError{
        .code = CallbackCheckerInitialValueErrorCode::UNTYPED_INITIAL,
        .message = message,
    });
  }

  return ok(accumulator);
}

CallbackChecker::ResultCallbackHasValue
CallbackChecker::checkMappedValue(const Type *result, llvm::StringRef name) const {
  if (result->isVoid() || result->isUndefined()) {
    auto message = ErrorTextCallbackMethod::noValue(name);

    return err(CallbackCheckerMappedValueError{
        .code = CallbackCheckerMappedValueErrorCode::NO_VALUE,
        .message = message,
    });
  }

  return ok(result);
}

CallbackChecker::ResultComparisonNumber CallbackChecker::checkComparison(const Type *result) const {
  if (!result->isNumeric() && !result->isDecimal()) {
    auto message = ErrorTextCallbackMethod::notNumber(result->name());

    return err(CallbackCheckerComparisonError{
        .code = CallbackCheckerComparisonErrorCode::NOT_A_NUMBER,
        .message = message,
    });
  }

  return ok(result);
}

Expr *CallbackChecker::checkCallbackMethod(CallExpr *call, MemberExpr *member,
                                           const Type *expected) {
  auto passed = this->checkCallbackGiven(call->args.size(), member->name);
  if (passed.isErr()) {
    this->state.getDiagnostics().error(member->loc(), passed.error().message);
    return call;
  }
  const Type *arrayType = member->object->type;
  bool isTyped = arrayType->isTypedArray();
  const auto *method = ArrayMethodTable::find(member->name);
  const CallbackArrayMethod *callbackMethod =
      method != nullptr ? method->form.as<CallbackArrayMethod>() : nullptr;
  ArrayCallback kind =
      callbackMethod != nullptr ? callbackMethod->callback : ArrayCallback::COMPARE;
  const Type *element = isTyped ? arrayType->elementType() : arrayType->inner();
  bool isReduce = kind == ArrayCallback::REDUCE;
  this->walkExtraCallbackArgs(call, member, isReduce);
  size_t count = call->args.size();
  bool hasSomeValue =
      !isReduce || count == 2 || this->indexProofs->canProveNotEmpty(member->object);
  const Type *accumulator =
      isReduce && count == 2 ? this->reduceInitType(call, member, element) : element;
  if (accumulator->isError()) {
    call->args[0] = this->exprChecker->checkExpr(call->args[0]);
    return call;
  }
  auto shape = ArrayCallbackShape{
      .call = call,
      .member = member,
      .arrayType = arrayType,
      .element = element,
      .kind = kind,
      .isTyped = isTyped,
      .count = count,
      .accumulator = accumulator,
      .hasSomeValue = hasSomeValue,
      .isBoolean = this->callChecker->isGlobalName(call->args[0], GlobalName::BOOLEAN),
  };
  auto signature = this->callbackSignature(shape, expected);
  this->rememberCallbackFacts(shape);
  unsigned primary = isReduce || kind == ArrayCallback::COMPARE ? 2 : 1;
  call->args[0] = this->checkFunctionArg(call->args[0], signature, primary, member->name);
  Builtin builtin = callbackMethod != nullptr ? method->builtin : Builtin::ArrSortWith;
  call->target = BuiltinCall{.builtin = builtin};
  this->flowChecker->calledFunction();
  if (!call->args[0]->type->isFunction()) {
    return call;
  }

  return this->checkArrayCallbackResult(shape);
}

void CallbackChecker::walkExtraCallbackArgs(CallExpr *call, const MemberExpr *member,
                                            bool isReduce) {
  size_t count = call->args.size();
  size_t maxArgs = isReduce ? 2 : 1;
  auto extra = this->checkExtraArguments(count, maxArgs, member->name, isReduce);
  if (extra.isOk()) {
    return;
  }
  bool isThisArg = extra.error().code == CallbackCheckerExtraArgumentsErrorCode::THIS_ARGUMENT;
  llvm::SMLoc at = isThisArg ? call->args[1]->loc() : member->loc();
  this->state.getDiagnostics().error(at, extra.error().message);
  for (size_t index = maxArgs; index < count; ++index) {
    call->args[index] = this->exprChecker->checkExpr(call->args[index]);
  }
  call->args.resize(maxArgs);
}

const Type *CallbackChecker::reduceInitType(CallExpr *call, const MemberExpr *member,
                                            const Type *element) {
  Expr *init = call->args[1];
  auto *unary = dyn_cast<UnaryExpr>(init);
  bool isNegative =
      unary != nullptr && unary->op == UnaryOp::Neg && isa<IntLitExpr>(unary->operand);
  bool isLiteral = isa<IntLitExpr>(init) || isNegative;
  const auto *elementType = this->genericChecker->apparentType(element);
  bool isDecimalStart = elementType->isDecimal() && this->decimalChecker->isLiteralTree(init);
  bool isWide = elementType->isInt() || elementType->isFloat();
  const Type *hint = isDecimalStart || (isLiteral && isWide) ? elementType : nullptr;
  call->args[1] = this->exprChecker->checkExpr(init, hint);
  const Type *accumulator = call->args[1]->type;
  auto initial = this->checkInitialValue(accumulator, member->name);
  if (initial.isErr()) {
    this->state.getDiagnostics().error(call->args[1]->loc(), initial.error().message);
    return Type::error();
  }

  return accumulator;
}

Signature CallbackChecker::callbackSignature(const ArrayCallbackShape &shape,
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

const Type *CallbackChecker::callbackReturn(const ArrayCallbackShape &shape,
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

const Type *CallbackChecker::mappedReturn(const ArrayCallbackShape &shape,
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

std::vector<const Type *> CallbackChecker::callbackParams(const ArrayCallbackShape &shape) const {
  if (shape.kind == ArrayCallback::COMPARE) {
    return {shape.element, shape.element};
  }
  if (shape.kind == ArrayCallback::REDUCE) {
    return {shape.accumulator, shape.element, Type::intTy(), shape.arrayType};
  }

  return {shape.element, Type::intTy(), shape.arrayType};
}

void CallbackChecker::rememberCallbackFacts(const ArrayCallbackShape &shape) {
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

void CallbackChecker::rememberIndexFacts(const ArrayCallbackShape &shape, FunctionDecl *callback) {
  bool isReduce = shape.kind == ArrayCallback::REDUCE;
  unsigned indexParam = isReduce ? 2 : 1;
  this->state.getCallbackFacts().rememberIndexCallback(callback, indexParam);
  auto *literal = dyn_cast<ArrayLitExpr>(shape.member->object);
  if (literal == nullptr || !shape.element->isInt()) {
    return;
  }
  unsigned elementParam = isReduce ? 1 : 0;
  auto elements = this->literalElements(literal, elementParam);
  if (elements.has_value()) {
    this->state.getCallbackFacts().rememberElementCallback(callback, *elements);
  }
}

void CallbackChecker::rememberByteFacts(const ArrayCallbackShape &shape, FunctionDecl *callback) {
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
  this->state.getCallbackFacts().rememberByteCallback(callback, info);
}

std::optional<ElementCallback> CallbackChecker::literalElements(const ArrayLitExpr *literal,
                                                                unsigned param) {
  std::optional<int64_t> low;
  std::optional<int64_t> high;
  for (const ArrayLitExpr::Element &element : literal->elements) {
    std::optional<int64_t> value = element.spread || element.hole
                                       ? std::nullopt
                                       : this->constantFolder->intConstant(element.expr);
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

Expr *CallbackChecker::checkArrayCallbackResult(const ArrayCallbackShape &shape) {
  CallExpr *call = shape.call;
  const Type *result = call->args[0]->type->ret();
  switch (shape.kind) {
    case ArrayCallback::FOR_EACH:
      call->type = Type::voidTy();
      return call;
    case ArrayCallback::MAP:
    case ArrayCallback::FLAT_MAP:
      return this->checkMappedResult(shape, result);
    case ArrayCallback::FILTER:
      call->type = this->filteredArray(shape);
      return call;
    case ArrayCallback::REDUCE:
      return this->finishArrayReduce(shape);
    case ArrayCallback::FIND:
      call->type = Type::optional(shape.element);
      return call;
    case ArrayCallback::FIND_INDEX:
      call->type = Type::intTy();
      return call;
    case ArrayCallback::TEST:
      call->type = Type::boolTy();
      return call;
    case ArrayCallback::COMPARE:
      return this->checkCompareResult(shape, result);
  }

  return call;
}

Expr *CallbackChecker::checkMappedResult(const ArrayCallbackShape &shape, const Type *result) {
  CallExpr *call = shape.call;
  auto value = this->checkMappedValue(result, shape.member->name);
  if (value.isErr()) {
    this->state.getDiagnostics().error(call->args[0]->loc(), value.error().message);
    return call;
  }
  if (shape.isTyped) {
    call->type = shape.arrayType;
    return call;
  }
  bool isFlat = shape.kind == ArrayCallback::FLAT_MAP && result->isArray();
  call->type = isFlat ? result : Type::array(result);

  return call;
}

const Type *CallbackChecker::filteredArray(const ArrayCallbackShape &shape) {
  const Expr *predicate = shape.call->args[0];
  const Type *element = shape.element;
  const auto *kept = this->predicateChecker->filteredType(predicate, element);
  if (kept != nullptr) {
    return Type::array(kept);
  }
  bool isDefined = element->isOptional() && (shape.isBoolean || this->isKeepingDefined(predicate));

  return isDefined ? Type::array(element->inner()) : shape.arrayType;
}

Expr *CallbackChecker::finishArrayReduce(const ArrayCallbackShape &shape) {
  CallExpr *call = shape.call;
  const Type *accumulator = shape.accumulator;
  call->type = shape.hasSomeValue ? accumulator : Type::optional(accumulator);
  call->mayBeOutOfRange = !shape.hasSomeValue;
  if (shape.count == 2 && call->args.size() == 2) {
    call->args[1] = this->typeRelations->coerce(call->args[1], accumulator);
  }

  return call;
}

Expr *CallbackChecker::checkCompareResult(const ArrayCallbackShape &shape, const Type *result) {
  CallExpr *call = shape.call;
  auto number = this->checkComparison(result);
  if (number.isErr()) {
    this->state.getDiagnostics().error(call->args[0]->loc(), number.error().message);
    return call;
  }
  call->type = shape.arrayType;

  return call;
}

}
