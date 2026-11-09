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
  this->callChecker = &all.getCallChecker();
  this->exprChecker = &all.getExprChecker();
  this->flowChecker = &all.getFlowChecker();
  this->indexProofs = &all.getIndexProofs();
  this->typeRelations = &all.getTypeRelations();
  this->closureChecker = &all.getClosureChecker();
  this->conditionFacts = &all.getConditionFacts();
  this->constantFolder = &all.getConstantFolder();
  this->genericChecker = &all.getGenericChecker();
  this->nestedFunctions = &all.getNestedFunctions();
  this->predicateChecker = &all.getPredicateChecker();
  this->genericInstantiator = &all.getGenericInstantiator();
  this->functionValueChecker = &all.getFunctionValueChecker();
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
  if (auto *fe = dyn_cast<FunctionExpr>(arg)) {
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
  if (FunctionExpr *wrapper =
          this->functionValueChecker->builtinFunctionValue(arg, expected, primary, method)) {
    wrapper->fn->nonEscaping = true;
    return this->closureChecker->checkFunctionExpr(wrapper, &expected);
  }
  this->state.getFunctionStack().enterCallback(isa<MemberExpr>(arg) ? &expected : nullptr);
  auto *named = dyn_cast<NameExpr>(arg);
  if (FuncSymbol *generic = named ? this->genericChecker->genericNamed(named) : nullptr) {
    arg = this->genericInstantiator->checkGenericValue(named, generic, &expected);
  } else {
    arg = this->exprChecker->checkExpr(arg);
  }
  this->state.getFunctionStack().leaveCallback();
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
  llvm::StringRef knownName = known ? known->name : llvm::StringRef();
  std::string name = ErrorTextFunctionName::quoted(knownName);
  ResultWithoutRestParam fixed = CallbackChecker::fixedParams(type, name);
  if (fixed.isErr()) {
    this->state.getDiagnostics().error(arg->loc(), fixed.error().message);
    return arg;
  }
  size_t passed = std::min(type->params().size(), expected.params.size());
  for (size_t i = 0; i < passed; ++i) {
    ResultParameterMatches param =
        CallbackChecker::callbackParam(type, expected, i, primary, known, name, method);
    if (param.isErr()) {
      this->state.getDiagnostics().error(arg->loc(), param.error().message);
      return arg;
    }
  }
  ResultArgumentsSuffice enough = CallbackChecker::argumentsEnough(type, expected, name, method);
  if (enough.isErr()) {
    this->state.getDiagnostics().error(arg->loc(), enough.error().message);
    return arg;
  }
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
  const Type *element = typed ? arrayType->elementType() : arrayType->inner();
  llvm::StringRef name = member->name;
  const ArrayMethodInfo *method = ArrayMethodTable::find(name);
  const CallbackArrayMethod *callbackMethod = nullptr;
  if (method != nullptr) {
    callbackMethod = method->form.as<CallbackArrayMethod>();
  }
  ArrayCallback kind = ArrayCallback::COMPARE;
  if (callbackMethod != nullptr) {
    kind = callbackMethod->callback;
  }
  size_t count = call->args.size();
  size_t maxArgs = kind == ArrayCallback::REDUCE ? 2 : 1;
  ResultCallbackIsPassed passed = CallbackChecker::callbackGiven(count, name);
  if (passed.isErr()) {
    this->state.getDiagnostics().error(member->loc(), passed.error().message);
    return call;
  }
  ResultNoExtraArguments extra =
      CallbackChecker::extraArguments(count, maxArgs, name, kind == ArrayCallback::REDUCE);
  if (extra.isErr()) {
    bool thisArg = extra.error().code == CallbackCheckerExtraArgumentsErrorCode::THIS_ARGUMENT;
    llvm::SMLoc at = thisArg ? call->args[1]->loc() : member->loc();
    this->state.getDiagnostics().error(at, extra.error().message);
    for (size_t i = maxArgs; i < count; ++i) {
      call->args[i] = this->exprChecker->checkExpr(call->args[i]);
    }
    call->args.resize(maxArgs);
    count = maxArgs;
  }

  Signature sig;
  sig.pad = false;
  unsigned primary = 1;
  const Type *accumulator = element;
  bool someValue = kind != ArrayCallback::REDUCE || count == 2 ||
                   this->indexProofs->provesNotEmpty(member->object);
  switch (kind) {
    case ArrayCallback::FOR_EACH:
      sig.voidResult = true;
      break;
    case ArrayCallback::MAP:
    case ArrayCallback::FLAT_MAP:
      if (const Type *wanted = expected ? expected->nonOptional() : nullptr;
          wanted && wanted->isArray() &&
          (wanted->inner()->holdsObjects() || wanted->inner()->holdsResults() ||
           (kind == ArrayCallback::MAP && wanted->inner()->nonOptional()->isArray()))) {
        sig.ret = kind == ArrayCallback::MAP ? wanted->inner() : wanted;
      }
      if (typed) {
        sig.ret = element;
      }
      break;
    case ArrayCallback::FILTER:
    case ArrayCallback::FIND:
    case ArrayCallback::FIND_INDEX:
    case ArrayCallback::TEST:
      sig.ret = Type::boolTy();
      break;
    case ArrayCallback::REDUCE:
      primary = 2;
      if (count == 2) {
        Expr *&init = call->args[1];
        auto *unary = dyn_cast<UnaryExpr>(init);
        bool literal = isa<IntLitExpr>(init) ||
                       (unary && unary->op == UnaryOp::Neg && isa<IntLitExpr>(unary->operand));
        const Type *elementType = GenericChecker::apparentType(element);
        bool decimalStart = elementType->isDecimal() && DecimalChecker::isLiteralTree(init);
        bool wide = elementType->isInt() || elementType->isFloat();
        init = this->exprChecker->checkExpr(init, decimalStart || (literal && wide) ? elementType
                                                                                    : nullptr);
        accumulator = init->type;
        ResultInitialValueType initial = CallbackChecker::initialValue(accumulator, name);
        if (initial.isErr()) {
          this->state.getDiagnostics().error(init->loc(), initial.error().message);
          accumulator = Type::error();
        }
      }
      sig.ret = accumulator;
      break;
    case ArrayCallback::COMPARE:
      primary = 2;
      break;
  }
  if (accumulator->isError()) {
    call->args[0] = this->exprChecker->checkExpr(call->args[0]);
    return call;
  }
  if (kind == ArrayCallback::COMPARE) {
    sig.params = {element, element};
  } else if (kind == ArrayCallback::REDUCE) {
    sig.params = {accumulator, element, Type::intTy(), arrayType};
  } else {
    sig.params = {element, Type::intTy(), arrayType};
  }

  bool isBoolean = this->callChecker->isGlobalName(call->args[0], GlobalName::BOOLEAN);
  if (auto *fe = dyn_cast<FunctionExpr>(call->args[0]); fe && kind != ArrayCallback::COMPARE) {
    this->state.getCallbackFacts().rememberIndexCallback(fe->fn,
                                                         kind == ArrayCallback::REDUCE ? 2 : 1);
    if (auto *lit = dyn_cast<ArrayLitExpr>(member->object); lit && element->isInt()) {
      std::optional<int64_t> lo, hi;
      for (const ArrayLitExpr::Element &e : lit->elements) {
        std::optional<int64_t> c =
            e.spread || e.hole ? std::nullopt : this->constantFolder->intConstant(e.expr);
        if (!c) {
          lo.reset();
          break;
        }
        lo = lo ? std::min(*lo, *c) : *c;
        hi = hi ? std::max(*hi, *c) : *c;
      }
      if (lo) {
        this->state.getCallbackFacts().rememberElementCallback(
            fe->fn, {kind == ArrayCallback::REDUCE ? 1u : 0u, *lo, *hi});
      }
    }
  }
  if (auto *fe = dyn_cast<FunctionExpr>(call->args[0]); fe && typed) {
    ByteCallback info;
    info.param = kind == ArrayCallback::REDUCE ? 1 : 0;
    arrayType->elementRange(info.lo, info.hi);
    if (kind == ArrayCallback::MAP) {
      info.target = arrayType;
    }
    this->state.getCallbackFacts().rememberByteCallback(fe->fn, info);
  }
  call->args[0] = this->checkFunctionArg(call->args[0], sig, primary, name);
  const Type *callback = call->args[0]->type;
  Builtin builtin = Builtin::ArrSortWith;
  if (callbackMethod != nullptr) {
    builtin = method->builtin;
  }
  call->target = BuiltinCall{.builtin = builtin};
  this->flowChecker->calledFunction();
  if (!callback->isFunction()) {
    return call;
  }
  const Type *result = callback->ret();
  switch (kind) {
    case ArrayCallback::FOR_EACH:
      call->type = Type::voidTy();
      break;
    case ArrayCallback::MAP:
    case ArrayCallback::FLAT_MAP: {
      ResultCallbackHasValue value = CallbackChecker::mappedValue(result, name);
      if (value.isErr()) {
        this->state.getDiagnostics().error(call->args[0]->loc(), value.error().message);
        return call;
      }
      if (typed) {
        call->type = arrayType;
        break;
      }
      call->type =
          kind == ArrayCallback::FLAT_MAP && result->isArray() ? result : Type::array(result);
      break;
    }
    case ArrayCallback::FILTER:
      if (const Type *kept = this->predicateChecker->filteredType(call->args[0], element)) {
        call->type = Type::array(kept);
      } else {
        call->type = element->isOptional() && (isBoolean || this->keepsDefined(call->args[0]))
                         ? Type::array(element->inner())
                         : arrayType;
      }
      break;
    case ArrayCallback::REDUCE:
      call->type = someValue ? accumulator : Type::optional(accumulator);
      call->mayBeOutOfRange = !someValue;
      if (count == 2 && call->args.size() == 2) {
        call->args[1] = this->typeRelations->coerce(call->args[1], accumulator);
      }
      break;
    case ArrayCallback::FIND:
      call->type = Type::optional(element);
      break;
    case ArrayCallback::FIND_INDEX:
      call->type = Type::intTy();
      break;
    case ArrayCallback::TEST:
      call->type = Type::boolTy();
      break;
    case ArrayCallback::COMPARE: {
      ResultComparisonNumber number = CallbackChecker::comparison(result);
      if (number.isErr()) {
        this->state.getDiagnostics().error(call->args[0]->loc(), number.error().message);
        return call;
      }
      call->type = arrayType;
      break;
    }
  }

  return call;
}

}
