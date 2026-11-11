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
  this->nestedFunctions = &all.nestedFunctions;
  this->predicateChecker = &all.predicateChecker;
  this->genericInstantiator = &all.genericInstantiator;
  this->functionValueChecker = &all.functionValueChecker;
}

CallbackChecker::ResultFunctionIsPassed CallbackChecker::functionGiven(const Type *type,
                                                                       llvm::StringRef method) {
  if (!type->isFunction()) {
    std::string message = ErrorTextCallbackArg::notFunc(method, type->name());

    return err(CallbackCheckerFunctionGivenError{
        .code = CallbackCheckerFunctionGivenErrorCode::NOT_A_FUNCTION,
        .message = message,
    });
  }

  return ok(type);
}

CallbackChecker::ResultWithoutRestParam CallbackChecker::fixedParams(const Type *type,
                                                                     llvm::StringRef name) {
  if (type->hasRest()) {
    std::string message = ErrorTextCallbackArg::rest(name);

    return err(CallbackCheckerFixedParamsError{
        .code = CallbackCheckerFixedParamsErrorCode::REST_PARAMETER,
        .message = message,
    });
  }

  return ok(type);
}

CallbackChecker::ResultParameterMatches
CallbackChecker::callbackParam(const Type *type, const Signature &expected, size_t i,
                               unsigned primary, const FunctionDecl *known, llvm::StringRef name,
                               llvm::StringRef method) {
  const Type *param = type->params()[i];
  const Type *given = expected.params[i];
  bool fits = param == given || (param->isOptional() && param->inner() == given);
  if (!fits) {
    std::string position = ErrorTextCallbackArg::position(i);
    std::string message =
        ErrorTextCallbackArg::paramType(name, param->name(), position, method, given->name());

    return err(CallbackCheckerCallbackParamError{
        .code = CallbackCheckerCallbackParamErrorCode::PARAM_TYPE,
        .message = message,
    });
  }
  if (i >= primary && i >= type->minArgs()) {
    bool named = known && i < known->params.size();
    std::string paramName = named ? ErrorTextCallbackArg::paramName(known->params[i].name) : "";
    std::string argName = ErrorTextCallbackArg::argName(method, i);
    std::string args = ErrorTextCallbackArg::argsShape(method);
    std::string message = ErrorTextCallbackArg::extraParam(name, argName, paramName, method, args);

    return err(CallbackCheckerCallbackParamError{
        .code = CallbackCheckerCallbackParamErrorCode::EXTRA_OPTIONAL,
        .message = message,
    });
  }

  return ok(param);
}

CallbackChecker::ResultArgumentsSuffice CallbackChecker::argumentsEnough(const Type *type,
                                                                         const Signature &expected,
                                                                         llvm::StringRef name,
                                                                         llvm::StringRef method) {
  if (type->minArgs() > expected.params.size()) {
    unsigned count = unsigned(expected.params.size());
    std::string passes = ErrorTextArity::wanted(count, count);
    std::string needs = std::to_string(type->minArgs());
    std::string message = ErrorTextCallbackArg::tooFew(method, passes, name, needs);

    return err(CallbackCheckerArgumentsEnoughError{
        .code = CallbackCheckerArgumentsEnoughErrorCode::TOO_FEW_PASSED,
        .message = message,
    });
  }

  return ok(type);
}

CallbackChecker::ResultReturnedTypeFits CallbackChecker::returnMatches(const Type *type,
                                                                       const Signature &expected,
                                                                       llvm::StringRef name,
                                                                       llvm::StringRef method) {
  if (expected.ret && type->ret() != expected.ret && !expected.voidResult) {
    std::string message =
        ErrorTextCallbackArg::badReturn(method, expected.ret->name(), name, type->ret()->name());

    return err(CallbackCheckerReturnMatchesError{
        .code = CallbackCheckerReturnMatchesErrorCode::WRONG_RETURN,
        .message = message,
    });
  }

  return ok(type);
}

CallbackChecker::ResultNothingIsDropped CallbackChecker::resultKept(const Type *type,
                                                                    const Signature &expected,
                                                                    llvm::StringRef name,
                                                                    llvm::StringRef method) {
  if (expected.voidResult && type->ret()->isResult()) {
    std::string message = ErrorTextCallbackArg::noCheck(name, method);

    return err(CallbackCheckerResultKeptError{
        .code = CallbackCheckerResultKeptErrorCode::RESULT_DROPPED,
        .message = message,
    });
  }
  if (expected.voidResult && !expected.asyncVoid && type->ret()->isPromise()) {
    std::string message = ErrorTextCallbackArg::noWait(name, method);

    return err(CallbackCheckerResultKeptError{
        .code = CallbackCheckerResultKeptErrorCode::PROMISE_DROPPED,
        .message = message,
    });
  }

  return ok(type);
}

Expr *CallbackChecker::checkFunctionArg(Expr *arg, const Signature &expected, unsigned primary,
                                        llvm::StringRef method) {
  auto *fe = dyn_cast<FunctionExpr>(arg);
  if (fe != nullptr) {
    return this->checkLiteralCallback(fe, expected, method);
  }
  FunctionExpr *wrapper =
      this->functionValueChecker->builtinFunctionValue(arg, expected, primary, method);
  if (wrapper != nullptr) {
    wrapper->fn->nonEscaping = true;
    return this->closureChecker->checkFunctionExpr(wrapper, &expected);
  }
  Expr *value = this->checkCallbackValue(arg, expected);

  return this->checkPassedFunction(value, expected, primary, method);
}

Expr *CallbackChecker::checkLiteralCallback(FunctionExpr *fe, const Signature &expected,
                                            llvm::StringRef method) {
  fe->fn->nonEscaping = true;
  Expr *checked = this->closureChecker->checkFunctionExpr(fe, &expected);
  FunctionValueChecker::ResultAsyncCallbackOk awaited =
      this->functionValueChecker->asyncCallback(checked, expected, method);
  if (awaited.isErr()) {
    this->state.getDiagnostics().error(checked->loc(), awaited.error().message);
    checked->type = Type::error();
  }

  return checked;
}

Expr *CallbackChecker::checkCallbackValue(Expr *arg, const Signature &expected) {
  this->state.getFunctionStack().enterCallback(isa<MemberExpr>(arg) ? &expected : nullptr);
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
  ResultFunctionIsPassed given = CallbackChecker::functionGiven(type, method);
  if (given.isErr()) {
    this->state.getDiagnostics().error(arg->loc(), given.error().message);
    return arg;
  }
  FunctionDecl *known = this->nestedFunctions->namedFunction(arg);
  llvm::StringRef knownName = known != nullptr ? known->name : llvm::StringRef();
  std::string name = ErrorTextFunctionName::quoted(knownName);
  ResultWithoutRestParam fixed = CallbackChecker::fixedParams(type, name);
  if (fixed.isErr()) {
    this->state.getDiagnostics().error(arg->loc(), fixed.error().message);
    return arg;
  }
  if (!this->callbackParamsFit(arg, expected, primary, known, name, method)) {
    return arg;
  }
  ResultArgumentsSuffice enough = CallbackChecker::argumentsEnough(type, expected, name, method);
  if (enough.isErr()) {
    this->state.getDiagnostics().error(arg->loc(), enough.error().message);
    return arg;
  }

  return this->checkCallbackResult(arg, expected, name, method);
}

bool CallbackChecker::callbackParamsFit(const Expr *arg, const Signature &expected,
                                        unsigned primary, const FunctionDecl *known,
                                        llvm::StringRef name, llvm::StringRef method) {
  const Type *type = arg->type;
  size_t passed = std::min(type->params().size(), expected.params.size());
  for (size_t i = 0; i < passed; ++i) {
    ResultParameterMatches param =
        CallbackChecker::callbackParam(type, expected, i, primary, known, name, method);
    if (param.isErr()) {
      this->state.getDiagnostics().error(arg->loc(), param.error().message);
      return false;
    }
  }

  return true;
}

Expr *CallbackChecker::checkCallbackResult(Expr *arg, const Signature &expected,
                                           llvm::StringRef name, llvm::StringRef method) {
  const Type *type = arg->type;
  FunctionValueChecker::ResultAsyncCallbackOk awaited =
      this->functionValueChecker->asyncCallback(arg, expected, method);
  if (awaited.isErr()) {
    this->state.getDiagnostics().error(arg->loc(), awaited.error().message);
    arg->type = Type::error();
    return arg;
  }
  ResultReturnedTypeFits returns = CallbackChecker::returnMatches(type, expected, name, method);
  if (returns.isErr()) {
    this->state.getDiagnostics().error(arg->loc(), returns.error().message);
    return arg;
  }
  ResultNothingIsDropped kept = CallbackChecker::resultKept(type, expected, name, method);
  if (kept.isErr()) {
    this->state.getDiagnostics().error(arg->loc(), kept.error().message);
  }

  return arg;
}

bool CallbackChecker::keepsDefined(const Expr *callback) const {
  auto *fe = dyn_cast<FunctionExpr>(callback);
  if (!fe || fe->fn->params.empty() || !fe->fn->params[0].sym || fe->fn->body->body.size() != 1) {
    return false;
  }
  auto *ret = dyn_cast<ReturnStmt>(fe->fn->body->body[0]);
  if (!ret || !ret->value) {
    return false;
  }
  SignedCondition whole{ret->value, true};
  std::vector<DefinedVars> answers;
  for (SignedCondition part : ChildrenFirst::order(whole, CallbackChecker::signedParts)) {
    DefinedVars vars = this->definedBy(part, answers);
    answers.push_back(std::move(vars));
  }

  return answers.back().contains(fe->fn->params[0].sym);
}

NodeChildren<SignedCondition> CallbackChecker::signedParts(SignedCondition part) {
  if (auto *unary = dyn_cast<UnaryExpr>(part.cond); unary && unary->op == UnaryOp::Not) {
    return {SignedCondition{unary->operand, !part.whenTrue}};
  }
  auto *bin = dyn_cast<BinaryExpr>(part.cond);
  bool logic = bin != nullptr && (bin->op == BinaryOp::LogAnd || bin->op == BinaryOp::LogOr);
  if (logic) {
    return {SignedCondition{bin->lhs, part.whenTrue}, SignedCondition{bin->rhs, part.whenTrue}};
  }

  return {};
}

CallbackChecker::DefinedVars CallbackChecker::definedBy(SignedCondition part,
                                                        std::vector<DefinedVars> &answers) const {
  if (auto *unary = dyn_cast<UnaryExpr>(part.cond); unary && unary->op == UnaryOp::Not) {
    return ChildrenFirst::takeAnswer(answers);
  }
  auto *bin = dyn_cast<BinaryExpr>(part.cond);
  bool logic = bin != nullptr && (bin->op == BinaryOp::LogAnd || bin->op == BinaryOp::LogOr);
  if (logic) {
    DefinedVars right = ChildrenFirst::takeAnswer(answers);
    DefinedVars left = ChildrenFirst::takeAnswer(answers);
    return CallbackChecker::joinDefined(bin->op == BinaryOp::LogAnd, part.whenTrue, left, right);
  }
  const Expr *cond = part.cond;
  if (auto *conv = dyn_cast<CastExpr>(cond); conv && conv->isImplicit() && conv->type->isBool()) {
    cond = conv->operand;
  }
  Flow whenTrue, whenFalse;
  this->conditionFacts->addConditionFacts(cond, whenTrue, whenFalse);

  return part.whenTrue ? whenTrue.defined : whenFalse.defined;
}

CallbackChecker::DefinedVars CallbackChecker::joinDefined(bool isAnd, bool whenTrue,
                                                          DefinedVars left,
                                                          const DefinedVars &right) {
  if (isAnd == whenTrue) {
    left.insert(right.begin(), right.end());
    return left;
  }
  DefinedVars both;
  for (const VarSymbol *var : left) {
    if (right.contains(var)) {
      both.insert(var);
    }
  }

  return both;
}

CallbackChecker::ResultCallbackIsPassed CallbackChecker::callbackGiven(size_t count,
                                                                       llvm::StringRef name) {
  if (count == 0) {
    std::string message = ErrorTextCallbackMethod::noFunction(name);

    return err(CallbackCheckerCallbackGivenError{
        .code = CallbackCheckerCallbackGivenErrorCode::NO_FUNCTION,
        .message = message,
    });
  }

  return ok(count);
}

CallbackChecker::ResultNoExtraArguments
CallbackChecker::extraArguments(size_t count, size_t maxArgs, llvm::StringRef name, bool reduce) {
  if (count > maxArgs && count == 2 && !reduce) {
    std::string message = ErrorTextCallbackMethod::thisArg(name);

    return err(CallbackCheckerExtraArgumentsError{
        .code = CallbackCheckerExtraArgumentsErrorCode::THIS_ARGUMENT,
        .message = message,
    });
  }
  if (count > maxArgs) {
    std::string message = ErrorTextArity::count(1, unsigned(maxArgs), count);

    return err(CallbackCheckerExtraArgumentsError{
        .code = CallbackCheckerExtraArgumentsErrorCode::TOO_MANY_ARGUMENTS,
        .message = message,
    });
  }

  return ok(count);
}

CallbackChecker::ResultInitialValueType CallbackChecker::initialValue(const Type *accumulator,
                                                                      llvm::StringRef name) {
  if (accumulator->isUndefined() || accumulator->isVoid()) {
    std::string message = ErrorTextCallbackMethod::untyped(name);

    return err(CallbackCheckerInitialValueError{
        .code = CallbackCheckerInitialValueErrorCode::UNTYPED_INITIAL,
        .message = message,
    });
  }

  return ok(accumulator);
}

CallbackChecker::ResultCallbackHasValue CallbackChecker::mappedValue(const Type *result,
                                                                     llvm::StringRef name) {
  if (result->isVoid() || result->isUndefined()) {
    std::string message = ErrorTextCallbackMethod::noValue(name);

    return err(CallbackCheckerMappedValueError{
        .code = CallbackCheckerMappedValueErrorCode::NO_VALUE,
        .message = message,
    });
  }

  return ok(result);
}

CallbackChecker::ResultComparisonNumber CallbackChecker::comparison(const Type *result) {
  if (!result->isNumeric() && !result->isDecimal()) {
    std::string message = ErrorTextCallbackMethod::notNumber(result->name());

    return err(CallbackCheckerComparisonError{
        .code = CallbackCheckerComparisonErrorCode::NOT_A_NUMBER,
        .message = message,
    });
  }

  return ok(result);
}

Expr *CallbackChecker::checkCallbackMethod(CallExpr *call, MemberExpr *member,
                                           const Type *expected) {
  const Type *arrayType = member->object->type;
  bool typed = arrayType->isTypedArray();
  const ArrayMethodInfo *method = ArrayMethodTable::find(member->name);
  const CallbackArrayMethod *callbackMethod =
      method != nullptr ? method->form.as<CallbackArrayMethod>() : nullptr;
  ArrayCallbackShape shape{
      .call = call,
      .member = member,
      .arrayType = arrayType,
      .element = typed ? arrayType->elementType() : arrayType->inner(),
      .kind = callbackMethod != nullptr ? callbackMethod->callback : ArrayCallback::COMPARE,
      .typed = typed,
      .count = call->args.size(),
      .accumulator = nullptr,
      .someValue = false,
      .isBoolean = false,
  };
  ResultCallbackIsPassed passed = CallbackChecker::callbackGiven(shape.count, member->name);
  if (passed.isErr()) {
    this->state.getDiagnostics().error(member->loc(), passed.error().message);
    return call;
  }
  this->walkExtraCallbackArgs(shape);
  bool reduce = shape.kind == ArrayCallback::REDUCE;
  shape.someValue =
      !reduce || shape.count == 2 || this->indexProofs->provesNotEmpty(member->object);
  shape.accumulator = reduce && shape.count == 2 ? this->reduceInitType(shape) : shape.element;
  if (shape.accumulator->isError()) {
    call->args[0] = this->exprChecker->checkExpr(call->args[0]);
    return call;
  }
  Signature sig = CallbackChecker::callbackSignature(shape, expected);
  shape.isBoolean = this->callChecker->isGlobalName(call->args[0], GlobalName::BOOLEAN);
  this->rememberCallbackFacts(shape);
  unsigned primary = reduce || shape.kind == ArrayCallback::COMPARE ? 2 : 1;
  call->args[0] = this->checkFunctionArg(call->args[0], sig, primary, member->name);
  Builtin builtin = callbackMethod != nullptr ? method->builtin : Builtin::ArrSortWith;
  call->target = BuiltinCall{.builtin = builtin};
  this->flowChecker->calledFunction();
  if (!call->args[0]->type->isFunction()) {
    return call;
  }

  return this->checkArrayCallbackResult(shape);
}

void CallbackChecker::walkExtraCallbackArgs(ArrayCallbackShape &shape) {
  CallExpr *call = shape.call;
  bool reduce = shape.kind == ArrayCallback::REDUCE;
  size_t maxArgs = reduce ? 2 : 1;
  ResultNoExtraArguments extra =
      CallbackChecker::extraArguments(shape.count, maxArgs, shape.member->name, reduce);
  if (extra.isOk()) {
    return;
  }
  bool thisArg = extra.error().code == CallbackCheckerExtraArgumentsErrorCode::THIS_ARGUMENT;
  llvm::SMLoc at = thisArg ? call->args[1]->loc() : shape.member->loc();
  this->state.getDiagnostics().error(at, extra.error().message);
  for (size_t i = maxArgs; i < shape.count; ++i) {
    call->args[i] = this->exprChecker->checkExpr(call->args[i]);
  }
  call->args.resize(maxArgs);
  shape.count = maxArgs;
}

const Type *CallbackChecker::reduceInitType(const ArrayCallbackShape &shape) {
  CallExpr *call = shape.call;
  Expr *init = call->args[1];
  auto *unary = dyn_cast<UnaryExpr>(init);
  bool negative = unary != nullptr && unary->op == UnaryOp::Neg && isa<IntLitExpr>(unary->operand);
  bool literal = isa<IntLitExpr>(init) || negative;
  const Type *elementType = GenericChecker::apparentType(shape.element);
  bool decimalStart = elementType->isDecimal() && DecimalChecker::isLiteralTree(init);
  bool wide = elementType->isInt() || elementType->isFloat();
  const Type *hint = decimalStart || (literal && wide) ? elementType : nullptr;
  call->args[1] = this->exprChecker->checkExpr(init, hint);
  const Type *accumulator = call->args[1]->type;
  ResultInitialValueType initial = CallbackChecker::initialValue(accumulator, shape.member->name);
  if (initial.isErr()) {
    this->state.getDiagnostics().error(call->args[1]->loc(), initial.error().message);
    return Type::error();
  }

  return accumulator;
}

Signature CallbackChecker::callbackSignature(const ArrayCallbackShape &shape,
                                             const Type *expected) {
  ArrayCallback kind = shape.kind;
  Signature sig;
  sig.pad = false;
  if (kind == ArrayCallback::FOR_EACH) {
    sig.voidResult = true;
  }
  if (kind == ArrayCallback::MAP || kind == ArrayCallback::FLAT_MAP) {
    sig.ret = CallbackChecker::mappedReturn(shape, expected);
  }
  bool predicate = kind == ArrayCallback::FILTER || kind == ArrayCallback::FIND ||
                   kind == ArrayCallback::FIND_INDEX || kind == ArrayCallback::TEST;
  if (predicate) {
    sig.ret = Type::boolTy();
  }
  if (kind == ArrayCallback::REDUCE) {
    sig.ret = shape.accumulator;
  }
  sig.params = CallbackChecker::callbackParams(shape);

  return sig;
}

const Type *CallbackChecker::mappedReturn(const ArrayCallbackShape &shape, const Type *expected) {
  if (shape.typed) {
    return shape.element;
  }
  bool map = shape.kind == ArrayCallback::MAP;
  const Type *wanted = expected != nullptr ? expected->nonOptional() : nullptr;
  bool shaped = wanted != nullptr && wanted->isArray() &&
                (wanted->inner()->holdsObjects() || wanted->inner()->holdsResults() ||
                 (map && wanted->inner()->nonOptional()->isArray()));
  if (!shaped) {
    return nullptr;
  }

  return map ? wanted->inner() : wanted;
}

std::vector<const Type *> CallbackChecker::callbackParams(const ArrayCallbackShape &shape) {
  if (shape.kind == ArrayCallback::COMPARE) {
    return {shape.element, shape.element};
  }
  if (shape.kind == ArrayCallback::REDUCE) {
    return {shape.accumulator, shape.element, Type::intTy(), shape.arrayType};
  }

  return {shape.element, Type::intTy(), shape.arrayType};
}

void CallbackChecker::rememberCallbackFacts(const ArrayCallbackShape &shape) {
  auto *fe = dyn_cast<FunctionExpr>(shape.call->args[0]);
  if (fe == nullptr) {
    return;
  }
  bool reduce = shape.kind == ArrayCallback::REDUCE;
  if (shape.kind != ArrayCallback::COMPARE) {
    this->state.getCallbackFacts().rememberIndexCallback(fe->fn, reduce ? 2 : 1);
    auto *lit = dyn_cast<ArrayLitExpr>(shape.member->object);
    std::optional<ElementCallback> elements = lit != nullptr && shape.element->isInt()
                                                  ? this->literalElements(lit, reduce ? 1u : 0u)
                                                  : std::nullopt;
    if (elements.has_value()) {
      this->state.getCallbackFacts().rememberElementCallback(fe->fn, *elements);
    }
  }
  if (!shape.typed) {
    return;
  }
  ByteCallback info;
  info.param = reduce ? 1 : 0;
  shape.arrayType->elementRange(info.lo, info.hi);
  if (shape.kind == ArrayCallback::MAP) {
    info.target = shape.arrayType;
  }
  this->state.getCallbackFacts().rememberByteCallback(fe->fn, info);
}

std::optional<ElementCallback> CallbackChecker::literalElements(const ArrayLitExpr *lit,
                                                                unsigned param) {
  std::optional<int64_t> lo;
  std::optional<int64_t> hi;
  for (const ArrayLitExpr::Element &e : lit->elements) {
    std::optional<int64_t> c =
        e.spread || e.hole ? std::nullopt : this->constantFolder->intConstant(e.expr);
    if (!c.has_value()) {
      return std::nullopt;
    }
    lo = lo.has_value() ? std::min(*lo, *c) : *c;
    hi = hi.has_value() ? std::max(*hi, *c) : *c;
  }
  if (!lo.has_value()) {
    return std::nullopt;
  }

  return ElementCallback{
      .param = param,
      .lo = *lo,
      .hi = *hi,
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
  ResultCallbackHasValue value = CallbackChecker::mappedValue(result, shape.member->name);
  if (value.isErr()) {
    this->state.getDiagnostics().error(call->args[0]->loc(), value.error().message);
    return call;
  }
  if (shape.typed) {
    call->type = shape.arrayType;
    return call;
  }
  bool flat = shape.kind == ArrayCallback::FLAT_MAP && result->isArray();
  call->type = flat ? result : Type::array(result);

  return call;
}

const Type *CallbackChecker::filteredArray(const ArrayCallbackShape &shape) {
  const Expr *predicate = shape.call->args[0];
  const Type *element = shape.element;
  const Type *kept = this->predicateChecker->filteredType(predicate, element);
  if (kept != nullptr) {
    return Type::array(kept);
  }
  bool defined = element->isOptional() && (shape.isBoolean || this->keepsDefined(predicate));

  return defined ? Type::array(element->inner()) : shape.arrayType;
}

Expr *CallbackChecker::finishArrayReduce(const ArrayCallbackShape &shape) {
  CallExpr *call = shape.call;
  const Type *accumulator = shape.accumulator;
  call->type = shape.someValue ? accumulator : Type::optional(accumulator);
  call->mayBeOutOfRange = !shape.someValue;
  if (shape.count == 2 && call->args.size() == 2) {
    call->args[1] = this->typeRelations->coerce(call->args[1], accumulator);
  }

  return call;
}

Expr *CallbackChecker::checkCompareResult(const ArrayCallbackShape &shape, const Type *result) {
  CallExpr *call = shape.call;
  ResultComparisonNumber number = CallbackChecker::comparison(result);
  if (number.isErr()) {
    this->state.getDiagnostics().error(call->args[0]->loc(), number.error().message);
    return call;
  }
  call->type = shape.arrayType;

  return call;
}

}
