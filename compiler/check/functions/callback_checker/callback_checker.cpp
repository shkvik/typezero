#include "compiler/check/functions/callback_checker/callback_checker.h"
#include "compiler/check/expr/call_checker/call_checker.h"
#include "compiler/check/expr/expr_checker/expr_checker.h"
#include "compiler/check/flow/condition_facts/condition_facts.h"
#include "compiler/check/flow/flow_checker/flow_checker.h"
#include "compiler/check/flow/predicate_checker/predicate_checker.h"
#include "compiler/check/functions/closure_checker/closure_checker.h"
#include "compiler/check/functions/function_value_checker/function_value_checker.h"
#include "compiler/check/functions/nested_functions/nested_functions.h"
#include "compiler/check/generics/generic_checker/generic_checker.h"
#include "compiler/check/generics/generic_instantiator/generic_instantiator.h"
#include "compiler/check/types/type_relations/type_relations.h"
#include "compiler/library/decimal/decimal_checker/decimal_checker.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/ADT/StringSet.h"
#include <functional>

using llvm::cast;
using llvm::dyn_cast;
using llvm::dyn_cast_or_null;
using llvm::isa;

namespace tz {

namespace {

// How a callback of a built-in method is called, for messages.
const char *callbackArgs(llvm::StringRef method) {
  if (method == "reduce" || method == "reduceRight") {
    return "(accumulator, value, index, array)";
  }
  if (method == "sort") {
    return "(a, b)";
  }

  return "(value, index, array)";
}

const char *ordinal(size_t i) {
  static const char *const names[] = {"first", "second", "third", "fourth", "fifth"};

  return i < 5 ? names[i] : "next";
}

// What the arguments of a callback are, by position.
const char *callbackArgName(llvm::StringRef method, size_t i) {
  bool reduce = method == "reduce" || method == "reduceRight";
  if (reduce) {
    return i == 0 ? "accumulator" : i == 1 ? "value" : i == 2 ? "index" : "array";
  }

  return i == 0 ? "value" : i == 1 ? "index" : "array";
}

} // namespace

CallbackChecker::CallbackChecker(Checker &checker) : checker_(checker) {}

Expr *CallbackChecker::checkFunctionArg(Expr *arg, const Checker::Signature &expected,
                                        unsigned primary, llvm::StringRef method) {
  if (auto *fe = dyn_cast<FunctionExpr>(arg)) {
    // The method only calls it, during the call.
    fe->fn->nonEscaping = true;
    Expr *checked = checker_.closureChecker().checkFunctionExpr(fe, &expected);
    if (checker_.functionValueChecker().asyncCallbackProblem(checked, expected, method)) {
      checked->type = Type::error();
    }
    return checked;
  }
  if (FunctionExpr *wrapper =
          checker_.functionValueChecker().builtinFunctionValue(arg, expected, primary, method)) {
    wrapper->fn->nonEscaping = true;
    return checker_.closureChecker().checkFunctionExpr(wrapper, &expected);
  }
  // `lists.map(first)`: the instance for what the method passes.
  const Checker::Signature *outerCallback =
      std::exchange(checker_.callback_, isa<MemberExpr>(arg) ? &expected : nullptr);
  auto *named = dyn_cast<NameExpr>(arg);
  if (FuncSymbol *generic = named ? checker_.genericChecker().genericNamed(named) : nullptr) {
    arg = checker_.genericInstantiator().checkGenericValue(named, generic, &expected);
  } else {
    arg = checker_.exprChecker().checkExpr(arg);
  }
  checker_.callback_ = outerCallback;
  const Type *type = arg->type;
  if (type->isError()) {
    return arg;
  }
  if (!type->isFunction()) {
    checker_.diags_.error(arg->loc(),
                          "'" + method + "' needs a function, found '" + type->name() + "'");
    return arg;
  }
  FunctionDecl *known = checker_.nestedFunctions().namedFunction(arg);
  std::string name = known && !known->name.empty() ? "'" + known->name.str() + "'" : "the function";
  if (type->hasRest()) {
    checker_.diags_.error(arg->loc(),
                          name + " takes a rest parameter; pass an arrow that calls it instead");
    return arg;
  }
  llvm::ArrayRef<const Type *> params = type->params();
  size_t passed = std::min(params.size(), expected.params.size());
  for (size_t i = 0; i < passed; ++i) {
    const Type *param = params[i], *given = expected.params[i];
    if (param != given && !(param->isOptional() && param->inner() == given)) {
      checker_.diags_.error(arg->loc(), name + " takes '" + param->name() + "' as its " +
                                            ordinal(i) + " argument, but '" + method +
                                            "' passes '" + given->name() + "'");
      return arg;
    }
    // `map(parseInt)`: the index would land in an optional parameter, which
    // the function surely meant for something else.
    if (i >= primary && i >= type->minArgs()) {
      std::string paramName =
          known && i < known->params.size() ? " '" + known->params[i].name.str() + "'" : "";
      checker_.diags_.error(arg->loc(), name + " would also get the " + callbackArgName(method, i) +
                                            " in its optional parameter" + paramName + ": '" +
                                            method + "' calls it with " + callbackArgs(method) +
                                            "; pass an arrow that gives it only what it needs");
      return arg;
    }
  }
  if (type->minArgs() > expected.params.size()) {
    checker_.diags_.error(arg->loc(), "'" + method + "' passes " +
                                          argumentCount(unsigned(expected.params.size()),
                                                        unsigned(expected.params.size())) +
                                          ", but " + name + " needs " +
                                          std::to_string(type->minArgs()));
    return arg;
  }
  if (checker_.functionValueChecker().asyncCallbackProblem(arg, expected, method)) {
    arg->type = Type::error();
    return arg;
  }
  if (expected.ret && type->ret() != expected.ret && !expected.voidResult) {
    checker_.diags_.error(arg->loc(), "'" + method + "' needs a function that returns '" +
                                          expected.ret->name() + "', but " + name + " returns '" +
                                          type->ret()->name() + "'");
    return arg;
  }
  // `names.forEach(store.save)`: what it returns is dropped, a Result too.
  if (expected.voidResult && type->ret()->isResult()) {
    checker_.diags_.error(arg->loc(),
                          name + " returns a Result that '" + method +
                              "' drops, so an error in it would go unchecked; pass an arrow "
                              "that handles it, e.g. with '.match(...)'");
  }
  // And a Promise, which nothing would wait for.
  if (expected.voidResult && !expected.asyncVoid && type->ret()->isPromise()) {
    checker_.diags_.error(arg->loc(),
                          name + " returns a Promise that '" + method +
                              "' drops, so nothing waits for it to finish; loop with 'for "
                              "(const x of xs) await f(x)', or wait for all with 'await "
                              "Promise.all(xs.map(...))'");
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
  // The variables a condition shows to hold a value when it is `value`.
  std::function<llvm::SmallPtrSet<const VarSymbol *, 4>(const Expr *, bool)> defined =
      [&](const Expr *cond, bool value) -> llvm::SmallPtrSet<const VarSymbol *, 4> {
    if (auto *unary = dyn_cast<UnaryExpr>(cond); unary && unary->op == UnaryOp::Not) {
      return defined(unary->operand, !value);
    }
    if (auto *bin = dyn_cast<BinaryExpr>(cond);
        bin && (bin->op == BinaryOp::LogAnd || bin->op == BinaryOp::LogOr)) {
      llvm::SmallPtrSet<const VarSymbol *, 4> l = defined(bin->lhs, value),
                                              r = defined(bin->rhs, value);
      // Both sides hold when `&&` is true (`||` false); one of them otherwise.
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
    // The truth of a value: `x` is truthy only if it holds one.
    if (auto *conv = dyn_cast<CastExpr>(cond); conv && conv->isImplicit() && conv->type->isBool()) {
      cond = conv->operand;
    }
    Checker::Flow whenTrue, whenFalse;
    checker_.conditionFacts().addConditionFacts(cond, whenTrue, whenFalse);

    return value ? whenTrue.defined : whenFalse.defined;
  };

  return defined(ret->value, true).contains(fe->fn->params[0].sym);
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
  // A typed array gives its elements as numbers (sema_bytes.cpp).
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
  auto checkArgsLoosely = [&] {
    for (Expr *&arg : call->args) {
      arg = checker_.exprChecker().checkExpr(arg);
    }
  };
  size_t maxArgs = kind == Reduce ? 2 : 1;
  if (count == 0) {
    checker_.diags_.error(member->loc(), "'" + name + "' needs a function");
    return call;
  }
  if (count > maxArgs) {
    if (count == 2 && kind != Reduce) {
      checker_.diags_.error(call->args[1]->loc(),
                            "the second argument of '" + name +
                                "' sets 'this' for the callback, which is not supported");
    } else {
      checker_.diags_.error(member->loc(), "expected " + argumentCount(1, unsigned(maxArgs)) +
                                               ", but got " + llvm::Twine(count));
    }
    for (size_t i = maxArgs; i < count; ++i) {
      call->args[i] = checker_.exprChecker().checkExpr(call->args[i]);
    }
    call->args.resize(maxArgs);
    count = maxArgs;
  }

  Checker::Signature sig;
  sig.pad = false;
  unsigned primary = 1;
  const Type *accumulator = element;
  switch (kind) {
  case ForEach:
    sig.voidResult = true;
    break;
  case Map:
  case FlatMap:
    // Where an array of objects is wanted, the objects the callback makes
    // take their type from it; so do Results, `xs.map(ok)`. A typed array
    // stores the results in a new one of its kind.
    if (const Type *wanted = expected ? expected->nonOptional() : nullptr;
        wanted && wanted->isArray() &&
        (Checker::holdsObjects(wanted->inner()) || Checker::holdsResults(wanted->inner()))) {
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
      // `reduce(f, 0)` over doubles starts from the double 0, over decimals
      // from the decimal 0 (and `reduce(f, 0.5)` from the decimal 0.5).
      Expr *&init = call->args[1];
      auto *unary = dyn_cast<UnaryExpr>(init);
      bool literal = isa<IntLitExpr>(init) ||
                     (unary && unary->op == UnaryOp::Neg && isa<IntLitExpr>(unary->operand));
      const Type *elementType = GenericChecker::apparentType(element);
      bool decimalStart = elementType->isDecimal() && DecimalChecker::isLiteralTree(init);
      init = checker_.exprChecker().checkExpr(
          init, decimalStart || (literal && elementType->isNumeric()) ? elementType : nullptr);
      accumulator = init->type;
      if (accumulator->isUndefined() || accumulator->isVoid()) {
        checker_.diags_.error(
            init->loc(), "the initial value of '" + name +
                             "' needs a type; declare it in a "
                             "variable first, e.g. 'const start: int | undefined = undefined'");
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
    call->args[0] = checker_.exprChecker().checkExpr(call->args[0]);
    return call;
  }
  if (kind == Sort) {
    sig.params = {element, element};
  } else if (kind == Reduce) {
    sig.params = {accumulator, element, Type::intTy(), arrayType};
  } else {
    sig.params = {element, Type::intTy(), arrayType};
  }

  bool isBoolean = checker_.callChecker().isGlobalName(call->args[0], "Boolean");
  // The callback of a typed array knows the range of its element; that of
  // map must give what fits in the new one.
  if (auto *fe = dyn_cast<FunctionExpr>(call->args[0]); fe && typed) {
    Checker::ByteCallback info;
    info.param = kind == Reduce ? 1 : 0;
    arrayType->elementRange(info.lo, info.hi);
    if (kind == Map) {
      info.target = arrayType;
    }
    checker_.byteCallbacks_[fe->fn] = info;
  }
  call->args[0] = checkFunctionArg(call->args[0], sig, primary, name);
  const Type *callback = call->args[0]->type;
  call->builtin = method ? method->builtin : Builtin::ArrSortWith;
  checker_.flowChecker().calledFunction(); // the callback may change anything
  if (!callback->isFunction()) {
    return call;
  }
  const Type *result = callback->ret();
  switch (kind) {
  case ForEach:
    call->type = Type::voidTy();
    break;
  case Map:
  case FlatMap:
    if (result->isVoid() || result->isUndefined()) {
      checker_.diags_.error(call->args[0]->loc(),
                            "the callback of '" + name + "' must return a value");
      return call;
    }
    if (typed) { // see checkBytesMethod
      call->type = arrayType;
      break;
    }
    call->type = kind == FlatMap && result->isArray() ? result : Type::array(result);
    break;
  case Filter:
    // `filter(Boolean)` and `filter(x => x != undefined)` keep only the
    // elements that hold a value: `(T | undefined)[]` becomes `T[]`.
    if (const Type *kept = checker_.predicateChecker().filteredType(call->args[0], element)) {
      call->type = Type::array(kept); // what a function that tests keeps
    } else {
      call->type = element->isOptional() && (isBoolean || keepsDefined(call->args[0]))
                       ? Type::array(element->inner())
                       : arrayType;
    }
    break;
  case Reduce:
    call->type = accumulator;
    if (count == 2 && call->args.size() == 2) {
      call->args[1] = checker_.typeRelations().coerce(call->args[1], accumulator);
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
  case Sort:
    if (!result->isNumeric() && !result->isDecimal()) {
      checker_.diags_.error(call->args[0]->loc(),
                            "a comparison function must return a number (negative, zero "
                            "or positive), found '" +
                                result->name() + "'");
      return call;
    }
    call->type = arrayType;
    break;
  }

  return call;
}

} // namespace tz
