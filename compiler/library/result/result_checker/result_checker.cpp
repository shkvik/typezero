#include "compiler/library/result/result_checker/result_checker.h"
#include "compiler/check/async/async_checker/async_checker.h"
#include "compiler/check/expr/argument_checker/argument_checker.h"
#include "compiler/check/expr/call_checker/call_checker.h"
#include "compiler/check/expr/expr_checker/expr_checker.h"
#include "compiler/check/functions/closure_checker/closure_checker.h"
#include "compiler/check/types/type_relations/type_relations.h"
#include "compiler/library/result/result_async_checker/result_async_checker.h"
#include "compiler/library/result/result_types/result_types.h"
#include "compiler/library/result/safe_try_checker/safe_try_checker.h"
#include "llvm/ADT/STLExtras.h"

using llvm::cast;
using llvm::dyn_cast;
using llvm::dyn_cast_or_null;
using llvm::isa;

namespace tz {

namespace {

// What a function given to a method gets for a half: `ok()` holds undefined.
const Type *halfParam(const Type *half) { return half->isVoid() ? Type::undefinedTy() : half; }

// Whether a half can tell a value its type: a hint of never or void cannot.
bool givesType(const Type *half) { return !half->isNever() && !half->isVoid(); }

// The variable a Result comes from, for hints: `r` in `r.map(f)`.
std::string receiverText(const Expr *expr) {
  if (auto *conv = dyn_cast<CastExpr>(expr); conv && conv->isImplicit()) {
    expr = conv->operand;
  }
  auto *name = dyn_cast<NameExpr>(expr);

  return name && !name->name.starts_with("%") ? name->name.str() : "r";
}

// The value of an expression strips the conversion Checker adds to a narrowed
// variable or to a value given where another type is expected.
const Expr *unconverted(const Expr *expr) {
  while (auto *conv = dyn_cast<CastExpr>(expr)) {
    if (!conv->isImplicit()) {
      break;
    }
    expr = conv->operand;
  }

  return expr;
}

} // namespace

// Why the halves `a` and `b` of Results make no union, for the messages
// that say so: ": <the problem>", or empty.
static std::string noUnion(const Type *a, const Type *b) {
  if (a->isVoid() || b->isVoid()) {
    return ": one of them is nothing, 'ok()'";
  }
  std::string problem = unionProblem(Type::unionOf({a, b}));

  return problem.empty() ? "" : ": " + problem;
}

ResultChecker::ResultChecker(Checker &checker) : checker_(checker) {}

Expr *ResultChecker::checkResultCall(CallExpr *call, NameExpr *name, const Type *expected) {
  llvm::StringRef which = checker_.resultTypes().neverthrowName(name->name);
  if (which == "safeTry") {
    return checker_.safeTryChecker().checkSafeTry(call, expected);
  }
  if (which == "fromThrowable") {
    return checkFromThrowable(call, name->loc());
  }
  if (which == "okAsync" || which == "errAsync" || which == "fromPromise" ||
      which == "fromSafePromise" || which == "fromAsyncThrowable") {
    return checker_.resultAsyncChecker().checkResultAsyncCall(call, name, which,
                                                              expected); // sema_async.cpp
  }
  if (which != "ok" && which != "err") {
    return nullptr;
  }

  return checkOkErr(call, name, which == "ok", expected);
}

Expr *ResultChecker::checkOkErr(CallExpr *call, NameExpr *name, bool isOk, const Type *expected) {
  if (checker_.callChecker().rejectSpread(call) ||
      !checker_.argumentChecker().checkArity(call, name->loc(), 0, 1)) {
    return call;
  }

  // The type expected gives the half this call makes, and the other one.
  const Type *wanted = expected ? expected->nonOptional() : nullptr;
  if (wanted && !wanted->isResult()) {
    wanted = nullptr;
  }
  const Type *wantedHalf = wanted ? (isOk ? wanted->okType() : wanted->errType()) : nullptr;
  const Type *half = Type::voidTy(); // `ok()`
  if (!call->args.empty()) {
    Expr *&arg = call->args[0];
    bool hinted = wantedHalf && givesType(wantedHalf);
    arg = checker_.exprChecker().checkExpr(arg, hinted ? wantedHalf : nullptr);
    half = arg->type;
    if (half->isVoid()) {
      checker_.diags_.error(arg->loc(),
                            "an expression of type 'void' has no value to keep; write '" +
                                name->name + "()'");
      half = Type::error();
    } else if (hinted && TypeRelations::isImplicitlyConvertible(half, wantedHalf)) {
      arg = checker_.typeRelations().convert(arg, wantedHalf);
      half = wantedHalf;
    }
  }
  const Type *other = wanted ? (isOk ? wanted->errType() : wanted->okType()) : Type::never();
  call->builtin = isOk ? Builtin::ResultOk : Builtin::ResultErr;
  call->type = isOk ? Type::result(half, other) : Type::result(other, half);

  return call;
}

Expr *ResultChecker::checkResultStatic(CallExpr *call, MemberExpr *member) {
  auto *name = dyn_cast<NameExpr>(member->object);
  if (!name || call->optional || member->optional ||
      checker_.resultTypes().neverthrowName(name->name) != "Result") {
    return nullptr;
  }
  llvm::StringRef method = member->name;
  auto checkArgsLoosely = [&] {
    for (Expr *&arg : call->args) {
      arg = checker_.exprChecker().checkExpr(arg);
    }
  };
  if (method == "fromThrowable") {
    return checkFromThrowable(call, member->loc());
  }
  if (method != "combine" && method != "combineWithAllErrors") {
    checker_.diags_.error(member->loc(), "'" + name->name + "." + method +
                                             "' does not exist; Result has 'combine' and "
                                             "'combineWithAllErrors'");
    checkArgsLoosely();
    return call;
  }
  if (checker_.callChecker().rejectSpread(call) ||
      !checker_.argumentChecker().checkArity(call, member->loc(), 1, 1)) {
    return call;
  }
  bool all = method == "combineWithAllErrors";
  call->builtin = all ? Builtin::ResultCombineWithAllErrors : Builtin::ResultCombine;
  std::string what = name->name.str() + "." + method.str();
  // The values (or errors) would make an array of undefined.
  auto holds = [&](const Type *result, const Expr *at) {
    if (givesType(result->okType()) && !(all && result->errType()->isVoid())) {
      return true;
    }
    checker_.diags_.error(at->loc(),
                          "'" + what + "' needs Results that hold " +
                              (givesType(result->okType()) ? "errors" : "values") + ", found '" +
                              result->name() +
                              "'; check them one by one, e.g. with 'every(r => r.isOk())'");

    return false;
  };
  auto errorsOf = [&](const Type *error) {
    return !all || error->isNever() ? error : Type::array(error);
  };

  // A list of Results with values of different types is a tuple of them, as
  // neverthrow types it: `Result.combine([parseName(s), parseAge(s)])` is a
  // `Result<[string, int], E>`.
  Expr *&arg = call->args[0];
  if (auto *lit = dyn_cast<ArrayLitExpr>(arg);
      lit && !lit->elements.empty() &&
      llvm::none_of(lit->elements,
                    [](const ArrayLitExpr::Element &e) { return e.spread || e.hole; })) {
    std::vector<const Type *> results;
    for (ArrayLitExpr::Element &e : lit->elements) {
      e.expr = checker_.exprChecker().checkExpr(e.expr);
      results.push_back(e.expr->type);
    }
    if (llvm::any_of(results, [](const Type *t) { return t->isError(); })) {
      return call;
    }
    auto other = llvm::find_if(results, [](const Type *t) { return !t->isResult(); });
    if (other != results.end()) {
      checker_.diags_.error(lit->elements[other - results.begin()].expr->loc(),
                            "'" + what + "' takes Results, found '" + (*other)->name() + "'");
      return call;
    }
    // Values of one type make an array; their errors may differ, `E | F`.
    auto sameValues = [](const Type *a, const Type *b) -> const Type * {
      const Type *x = a->okType(), *y = b->okType();
      const Type *value = x == y || y->isNever()       ? x
                          : x->isNever()               ? y
                          : x->isVoid() || y->isVoid() ? nullptr
                                                       : TypeRelations::unify(x, y);
      const Type *error = ResultTypes::unifyHalf(a->errType(), b->errType());

      return value && !value->isError() && error ? Type::result(value, error) : nullptr;
    };
    const Type *common = results[0];
    for (const Type *t : results) {
      common = common ? sameValues(common, t) : nullptr;
    }
    if (common && !common->isError()) {
      // Of one type: an array.
      if (!holds(common, arg)) {
        return call;
      }
      for (ArrayLitExpr::Element &e : lit->elements) {
        e.expr = checker_.typeRelations().convert(e.expr, common);
      }
      lit->type = Type::array(common);
      call->type = Type::result(Type::array(common->okType()), errorsOf(common->errType()));
      return call;
    }
    std::vector<const Type *> values;
    const Type *error = Type::never();
    for (size_t i = 0; i < results.size(); ++i) {
      if (!holds(results[i], lit->elements[i].expr)) {
        return call;
      }
      values.push_back(results[i]->okType());
      const Type *before = error;
      if (!(error = ResultTypes::unifyHalf(error, results[i]->errType()))) {
        checker_.diags_.error(
            lit->elements[i].expr->loc(),
            "the Results given to '" + what + "' have errors that no union holds" +
                noUnion(before, results[i]->errType()) + "; convert them with 'mapErr'");

        return call;
      }
    }
    lit->type = Type::tuple(results);
    call->type = Type::result(Type::tuple(values), errorsOf(error));
    return call;
  }

  arg = checker_.exprChecker().checkExpr(arg);
  const Type *type = arg->type;
  if (type->isError()) {
    return call;
  }
  if (!type->isArray() || !type->inner()->isResult()) {
    checker_.diags_.error(arg->loc(),
                          "'" + what + "' takes an array of Results, found '" + type->name() + "'");
    return call;
  }
  const Type *element = type->inner();
  if (!holds(element, arg)) {
    return call;
  }
  call->type = Type::result(Type::array(element->okType()), errorsOf(element->errType()));

  return call;
}

Expr *ResultChecker::checkResultMember(MemberExpr *member) {
  const Type *type = member->object->type;
  llvm::StringRef name = member->name;
  std::string r = receiverText(member->object);
  if (name == "value") {
    if (type->errType()->isNever()) {
      member->type = halfParam(type->okType());
      return member;
    }
    checker_.diags_.error(member->loc(),
                          "'value' is there only when the Result is an Ok: check it first, 'if (" +
                              r + ".isOk())', or use 'match' or 'unwrapOr'");
  } else if (name == "error") {
    if (type->okType()->isNever()) {
      member->type = halfParam(type->errType());
      return member;
    }
    checker_.diags_.error(member->loc(),
                          "'error' is there only when the Result is an Err: check it first, 'if (" +
                              r + ".isErr())', or use 'match'");
  } else {
    static const char *const methods[] = {"isOk",    "isErr",  "map",       "mapErr",
                                          "andThen", "orElse", "match",     "unwrapOr",
                                          "andTee",  "orTee",  "andThrough"};
    if (llvm::is_contained(methods, name)) {
      checker_.diags_.error(member->loc(), "'" + name + "' is a method of Result; call it: '" + r +
                                               "." + name + "(...)'");
    } else {
      checker_.diags_.error(member->loc(), "property '" + name + "' does not exist on type '" +
                                               type->name() + "'");
    }
  }

  return member;
}

void ResultChecker::checkDiscarded(const Expr *expr, bool callback, llvm::SMLoc at) {
  const Type *type = expr->type;
  // A Promise nothing waits for (sema_async.cpp).
  if (type && Checker::holdsPromises(type)) {
    checker_.asyncChecker().checkDiscardedPromise(expr, callback, at);
    return;
  }
  // `r = ok(1)` keeps it.
  if (!type || !Checker::holdsResults(type) || isa<AssignExpr>(expr)) {
    return;
  }
  // What pop(), shift() and splice() take out of an array was kept there,
  // and is removed on purpose; reverse() and the like give the array itself.
  auto *call = dyn_cast<CallExpr>(expr);
  if (call && (call->builtin == Builtin::ArrPop || call->builtin == Builtin::ArrShift ||
               call->builtin == Builtin::ArrSplice)) {
    return;
  }
  if (!type->nonOptional()->isResult()) {
    // New Results nobody takes: `files.map(f => save(f))`.
    if (call && (call->builtin == Builtin::ArrMap || call->builtin == Builtin::ArrFlatMap)) {
      checker_.diags_.error(expr->loc(),
                            "the Results are not used, so errors in them would go unchecked; "
                            "handle them, e.g. with 'Result.combine(...)', or keep them in a "
                            "variable");
    }
    return;
  }
  if (callback) {
    checker_.diags_.error(expr->loc(),
                          "the function returns a Result that its caller drops, so an error in it "
                          "would go unchecked; handle it here, e.g. with '.match(...)'");
  } else {
    checker_.diags_.error(expr->loc(),
                          "the Result is not used, so an error in it would go unchecked; handle "
                          "it, e.g. with '.match(...)', or keep it in a variable");
  }
}

void ResultChecker::addResultFacts(const Expr *cond, Checker::Flow &whenTrue,
                                   Checker::Flow &whenFalse) const {
  auto *call = dyn_cast<CallExpr>(cond);
  if (!call || (call->builtin != Builtin::ResultIsOk && call->builtin != Builtin::ResultIsErr)) {
    return;
  }
  auto *name = dyn_cast<NameExpr>(unconverted(cast<MemberExpr>(call->callee)->object));
  if (!name || !name->var || !name->var->type || !name->var->type->isResult()) {
    return;
  }
  bool isOk = call->builtin == Builtin::ResultIsOk;
  Checker::Flow &ok = isOk ? whenTrue : whenFalse, &err = isOk ? whenFalse : whenTrue;
  ok.okResults.insert(name->var);
  ok.errResults.erase(name->var);
  err.errResults.insert(name->var);
  err.okResults.erase(name->var);
}

// `Result.fromThrowable(f, toError?)`: a function with the parameters of f
// that calls it and gives what it returns as an Ok, and what it throws as an
// Err, an Error or what toError makes of it. f and toError are evaluated once,
// here (see Lowering::emitFromThrowable).
Expr *ResultChecker::checkFromThrowable(CallExpr *call, llvm::SMLoc loc) {
  call->type = Type::error();
  if (checker_.callChecker().rejectSpread(call) ||
      !checker_.argumentChecker().checkArity(call, loc, 1, 2)) {
    return call;
  }
  Expr *&fn = call->args[0];
  fn = checker_.exprChecker().checkExpr(fn);
  if (call->args.size() > 1 && !isa<FunctionExpr>(call->args[1])) {
    call->args[1] = checker_.exprChecker().checkExpr(call->args[1]);
  }
  const Type *fnType = fn->type;
  if (fnType->isError()) {
    return call;
  }
  if (!fnType->isFunction()) {
    checker_.diags_.error(fn->loc(),
                          "'fromThrowable' takes a function, found '" + fnType->name() + "'");
    return call;
  }
  // An async function throws nothing: its errors reject its Promise.
  if (fnType->ret()->isPromise()) {
    checker_.diags_.error(fn->loc(),
                          "'fromThrowable' of a function that gives a Promise would give an Ok "
                          "with the Promise, as the function's errors reject the Promise; use "
                          "'ResultAsync.fromThrowable(f)', which gives a ResultAsync");
    return call;
  }
  const Type *errType = checker_.errorClass_->type;
  if (call->args.size() > 1) {
    Checker::Signature sig;
    sig.params = {checker_.errorClass_->type};
    sig.minArgs = 1;
    // It is kept in the function made here, so an arrow's environment
    // cannot live on the stack (as checkFunctionArg would have it).
    Expr *&toError = call->args[1];
    if (auto *fe = dyn_cast<FunctionExpr>(toError)) {
      toError = checker_.closureChecker().checkFunctionExpr(fe, &sig);
    }
    const Type *toType = toError->type;
    if (toType->isError()) {
      return call;
    }
    bool takesError =
        toType->isFunction() && toType->minArgs() <= 1 &&
        (toType->params().empty() || toType->params()[0] == checker_.errorClass_->type ||
         toType->params()[0] == Type::optional(checker_.errorClass_->type));
    if (!takesError) {
      checker_.diags_.error(toError->loc(),
                            "the second argument of 'fromThrowable' turns the Error into "
                            "the error of the Result: a function that takes an Error, e.g. "
                            "'(e) => e.message'");
      return call;
    }
    errType = toError->type->ret();
    if (errType->isVoid()) {
      checker_.diags_.error(toError->loc(),
                            "the function that turns the Error into the error of the "
                            "Result must return it");
      return call;
    }
  }
  call->builtin = Builtin::ResultFromThrowable;
  call->type = Type::function(fnType->params(), fnType->minArgs(), fnType->hasRest(),
                              Type::result(fnType->ret(), errType));

  return call;
}

} // namespace tz
