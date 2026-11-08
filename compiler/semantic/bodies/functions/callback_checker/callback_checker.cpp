#include "compiler/semantic/bodies/functions/callback_checker/callback_checker.h"
#include "compiler/semantic/bodies/expr/argument_checker/argument_checker_errors.h"
#include "compiler/semantic/bodies/expr/call_checker/call_checker.h"
#include "compiler/semantic/bodies/expr/expr_checker/expr_checker.h"
#include "compiler/semantic/bodies/functions/closure_checker/closure_checker.h"
#include "compiler/semantic/bodies/functions/function_checker/function_checker_errors.h"
#include "compiler/semantic/bodies/functions/function_value_checker/function_value_checker.h"
#include "compiler/semantic/bodies/functions/nested_functions/nested_functions.h"
#include "compiler/semantic/declare/constant_folder/constant_folder.h"
#include "compiler/semantic/flow/condition_facts/condition_facts.h"
#include "compiler/semantic/flow/flow_checker/flow_checker.h"
#include "compiler/semantic/flow/index_proofs/index_proofs.h"
#include "compiler/semantic/flow/predicate_checker/predicate_checker.h"
#include "compiler/semantic/generics/generic_checker/generic_checker.h"
#include "compiler/semantic/generics/generic_instantiator/generic_instantiator.h"
#include "compiler/semantic/library/decimal/decimal_checker/decimal_checker.h"
#include "compiler/semantic/types/type_relations/type_relations.h"
#include "llvm/ADT/STLExtras.h"
#include <functional>

using llvm::dyn_cast;
using llvm::isa;

namespace tz {

CallbackChecker::CallbackChecker(SemanticState &owner)
    : state(owner) {}

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
    Expr *checked = ClosureChecker(this->state).checkFunctionExpr(fe, &expected);
    FunctionValueChecker::ResultAsyncCallbackOk awaited =
        FunctionValueChecker(this->state).asyncCallback(checked, expected, method);
    if (awaited.isErr()) {
      this->state.getDiagnostics().error(checked->loc(), awaited.error().message);
      checked->type = Type::error();
    }
    return checked;
  }
  if (FunctionExpr *wrapper =
          FunctionValueChecker(this->state).builtinFunctionValue(arg, expected, primary, method)) {
    wrapper->fn->nonEscaping = true;
    return ClosureChecker(this->state).checkFunctionExpr(wrapper, &expected);
  }
  this->state.getFunctionStack().enterCallback(isa<MemberExpr>(arg) ? &expected : nullptr);
  auto *named = dyn_cast<NameExpr>(arg);
  if (FuncSymbol *generic = named ? GenericChecker(this->state).genericNamed(named) : nullptr) {
    arg = GenericInstantiator(this->state).checkGenericValue(named, generic, &expected);
  } else {
    arg = ExprChecker(this->state).checkExpr(arg);
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
  FunctionDecl *known = NestedFunctions(this->state).namedFunction(arg);
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
      FunctionValueChecker(this->state).asyncCallback(arg, expected, method);
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
  std::function<llvm::SmallPtrSet<const VarSymbol *, 4>(const Expr *, bool)> defined =
      [&](const Expr *cond, bool value) -> llvm::SmallPtrSet<const VarSymbol *, 4> {
    if (auto *unary = dyn_cast<UnaryExpr>(cond); unary && unary->op == UnaryOp::Not) {
      return defined(unary->operand, !value);
    }
    if (auto *bin = dyn_cast<BinaryExpr>(cond);
        bin && (bin->op == BinaryOp::LogAnd || bin->op == BinaryOp::LogOr)) {
      llvm::SmallPtrSet<const VarSymbol *, 4> l = defined(bin->lhs, value),
                                              r = defined(bin->rhs, value);
      if ((bin->op == BinaryOp::LogAnd) == value) {
        l.insert(r.begin(), r.end());
        return l;
      }
      llvm::SmallPtrSet<const VarSymbol *, 4> both;
      for (const VarSymbol *var : l) {
        if (r.contains(var)) {
          both.insert(var);
        }
      }
      return both;
    }
    if (auto *conv = dyn_cast<CastExpr>(cond); conv && conv->isImplicit() && conv->type->isBool()) {
      cond = conv->operand;
    }
    Flow whenTrue, whenFalse;
    ConditionFacts(this->state).addConditionFacts(cond, whenTrue, whenFalse);

    return value ? whenTrue.defined : whenFalse.defined;
  };

  return defined(ret->value, true).contains(fe->fn->params[0].sym);
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
  enum Kind { ForEach, Map, FlatMap, Filter, Reduce, Find, FindIndex, Test, Sort };
  struct Method {
    llvm::StringLiteral name;
    Builtin builtin;
    Kind kind;
  };
  static const Method methods[] = {
      {"forEach", Builtin::ArrForEach, ForEach},
      {"map", Builtin::ArrMap, Map},
      {"flatMap", Builtin::ArrFlatMap, FlatMap},
      {"filter", Builtin::ArrFilter, Filter},
      {"reduce", Builtin::ArrReduce, Reduce},
      {"reduceRight", Builtin::ArrReduceRight, Reduce},
      {"find", Builtin::ArrFind, Find},
      {"findLast", Builtin::ArrFindLast, Find},
      {"findIndex", Builtin::ArrFindIndex, FindIndex},
      {"findLastIndex", Builtin::ArrFindLastIndex, FindIndex},
      {"some", Builtin::ArrSome, Test},
      {"every", Builtin::ArrEvery, Test},
  };
  const Type *arrayType = member->object->type;
  bool typed = arrayType->isTypedArray();
  const Type *element = typed ? arrayType->elementType() : arrayType->inner();
  llvm::StringRef name = member->name;
  const Method *method = nullptr;
  for (const Method &m : methods) {
    if (m.name == name) {
      method = &m;
    }
  }
  Kind kind = method ? method->kind : Sort;
  size_t count = call->args.size();
  size_t maxArgs = kind == Reduce ? 2 : 1;
  ResultCallbackIsPassed passed = CallbackChecker::callbackGiven(count, name);
  if (passed.isErr()) {
    this->state.getDiagnostics().error(member->loc(), passed.error().message);
    return call;
  }
  ResultNoExtraArguments extra =
      CallbackChecker::extraArguments(count, maxArgs, name, kind == Reduce);
  if (extra.isErr()) {
    bool thisArg = extra.error().code == CallbackCheckerExtraArgumentsErrorCode::THIS_ARGUMENT;
    llvm::SMLoc at = thisArg ? call->args[1]->loc() : member->loc();
    this->state.getDiagnostics().error(at, extra.error().message);
    for (size_t i = maxArgs; i < count; ++i) {
      call->args[i] = ExprChecker(this->state).checkExpr(call->args[i]);
    }
    call->args.resize(maxArgs);
    count = maxArgs;
  }

  Signature sig;
  sig.pad = false;
  unsigned primary = 1;
  const Type *accumulator = element;
  bool someValue =
      kind != Reduce || count == 2 || IndexProofs(this->state).provesNotEmpty(member->object);
  switch (kind) {
  case ForEach:
    sig.voidResult = true;
    break;
  case Map:
  case FlatMap:
    if (const Type *wanted = expected ? expected->nonOptional() : nullptr;
        wanted && wanted->isArray() &&
        (wanted->inner()->holdsObjects() || wanted->inner()->holdsResults() ||
         (kind == Map && wanted->inner()->nonOptional()->isArray()))) {
      sig.ret = kind == Map ? wanted->inner() : wanted;
    }
    if (typed) {
      sig.ret = element;
    }
    break;
  case Filter:
  case Find:
  case FindIndex:
  case Test:
    sig.ret = Type::boolTy();
    break;
  case Reduce:
    primary = 2;
    if (count == 2) {
      Expr *&init = call->args[1];
      auto *unary = dyn_cast<UnaryExpr>(init);
      bool literal = isa<IntLitExpr>(init) ||
                     (unary && unary->op == UnaryOp::Neg && isa<IntLitExpr>(unary->operand));
      const Type *elementType = GenericChecker::apparentType(element);
      bool decimalStart = elementType->isDecimal() && DecimalChecker::isLiteralTree(init);
      bool wide = elementType->isInt() || elementType->isFloat();
      init = ExprChecker(this->state)
                 .checkExpr(init, decimalStart || (literal && wide) ? elementType : nullptr);
      accumulator = init->type;
      ResultInitialValueType initial = CallbackChecker::initialValue(accumulator, name);
      if (initial.isErr()) {
        this->state.getDiagnostics().error(init->loc(), initial.error().message);
        accumulator = Type::error();
      }
    }
    sig.ret = accumulator;
    break;
  case Sort:
    primary = 2;
    break;
  }
  if (accumulator->isError()) {
    call->args[0] = ExprChecker(this->state).checkExpr(call->args[0]);
    return call;
  }
  if (kind == Sort) {
    sig.params = {element, element};
  } else if (kind == Reduce) {
    sig.params = {accumulator, element, Type::intTy(), arrayType};
  } else {
    sig.params = {element, Type::intTy(), arrayType};
  }

  bool isBoolean = CallChecker(this->state).isGlobalName(call->args[0], "Boolean");
  if (auto *fe = dyn_cast<FunctionExpr>(call->args[0]); fe && kind != Sort) {
    this->state.getCallbackFacts().rememberIndexCallback(fe->fn, kind == Reduce ? 2 : 1);
    if (auto *lit = dyn_cast<ArrayLitExpr>(member->object); lit && element->isInt()) {
      std::optional<int64_t> lo, hi;
      for (const ArrayLitExpr::Element &e : lit->elements) {
        std::optional<int64_t> c =
            e.spread || e.hole ? std::nullopt : ConstantFolder(this->state).intConstant(e.expr);
        if (!c) {
          lo.reset();
          break;
        }
        lo = lo ? std::min(*lo, *c) : *c;
        hi = hi ? std::max(*hi, *c) : *c;
      }
      if (lo) {
        this->state.getCallbackFacts().rememberElementCallback(
            fe->fn, {kind == Reduce ? 1u : 0u, *lo, *hi});
      }
    }
  }
  if (auto *fe = dyn_cast<FunctionExpr>(call->args[0]); fe && typed) {
    ByteCallback info;
    info.param = kind == Reduce ? 1 : 0;
    arrayType->elementRange(info.lo, info.hi);
    if (kind == Map) {
      info.target = arrayType;
    }
    this->state.getCallbackFacts().rememberByteCallback(fe->fn, info);
  }
  call->args[0] = this->checkFunctionArg(call->args[0], sig, primary, name);
  const Type *callback = call->args[0]->type;
  call->target = BuiltinCall{.builtin = method ? method->builtin : Builtin::ArrSortWith};
  FlowChecker(this->state).calledFunction();
  if (!callback->isFunction()) {
    return call;
  }
  const Type *result = callback->ret();
  switch (kind) {
  case ForEach:
    call->type = Type::voidTy();
    break;
  case Map:
  case FlatMap: {
    ResultCallbackHasValue value = CallbackChecker::mappedValue(result, name);
    if (value.isErr()) {
      this->state.getDiagnostics().error(call->args[0]->loc(), value.error().message);
      return call;
    }
    if (typed) {
      call->type = arrayType;
      break;
    }
    call->type = kind == FlatMap && result->isArray() ? result : Type::array(result);
    break;
  }
  case Filter:
    if (const Type *kept = PredicateChecker(this->state).filteredType(call->args[0], element)) {
      call->type = Type::array(kept);
    } else {
      call->type = element->isOptional() && (isBoolean || this->keepsDefined(call->args[0]))
                       ? Type::array(element->inner())
                       : arrayType;
    }
    break;
  case Reduce:
    call->type = someValue ? accumulator : Type::optional(accumulator);
    call->mayBeOutOfRange = !someValue;
    if (count == 2 && call->args.size() == 2) {
      call->args[1] = TypeRelations(this->state).coerce(call->args[1], accumulator);
    }
    break;
  case Find:
    call->type = Type::optional(element);
    break;
  case FindIndex:
    call->type = Type::intTy();
    break;
  case Test:
    call->type = Type::boolTy();
    break;
  case Sort: {
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
