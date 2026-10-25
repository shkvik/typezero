#include "compiler/library/result/result_async_checker/result_async_checker.h"
#include "compiler/check/expr/argument_checker/argument_checker.h"
#include "compiler/check/expr/call_checker/call_checker.h"
#include "compiler/check/expr/expr_checker/expr_checker.h"
#include "compiler/check/functions/callback_checker/callback_checker.h"
#include "compiler/check/functions/closure_checker/closure_checker.h"
#include "compiler/check/types/type_relations/type_relations.h"
#include "compiler/library/result/result_checker/result_checker.h"
#include "compiler/library/result/result_types/result_types.h"
#include "llvm/ADT/STLExtras.h"

using llvm::cast;
using llvm::dyn_cast;
using llvm::dyn_cast_or_null;
using llvm::isa;

namespace tz {

namespace {

// What `await` of a value of `type` gives: the value of a Promise, or the
// value itself.
const Type *awaited(const Type *type) { return type->isPromise() ? type->promised() : type; }

// The Result a Promise of one that is expected holds.
const Type *wantedResult(const Type *expected) {
  const Type *type = expected ? expected->nonOptional() : nullptr;

  return type && type->isPromise() && type->promised()->isResult() ? type->promised() : nullptr;
}

// What a callback gets for a half of a Result: `ok()` holds undefined.
const Type *halfOf(const Type *half) { return half->isVoid() ? Type::undefinedTy() : half; }

} // namespace

ResultAsyncChecker::ResultAsyncChecker(Checker &checker) : checker_(checker) {}

Expr *ResultAsyncChecker::checkResultAsyncCall(CallExpr *call, NameExpr *name,
                                               llvm::StringRef which, const Type *expected) {
  checker_.file_->usesAsync = true;
  if (which == "fromPromise" || which == "fromSafePromise") {
    return checkResultFromPromise(call, name->loc(), which == "fromSafePromise", expected);
  }
  if (which == "fromAsyncThrowable") {
    return checkResultAsyncThrowable(call, name->loc());
  }
  // okAsync(v) and errAsync(e): ok(v) and err(e) in a Promise settled now.
  Expr *made =
      checker_.resultChecker().checkOkErr(call, name, which == "okAsync", wantedResult(expected));
  if (!made->type->isResult()) {
    return made;
  }
  auto *resolve =
      checker_.ctx_.make<CallExpr>(call->loc(), call->callee, std::vector<Expr *>{made});
  resolve->builtin = Builtin::PromiseResolve;
  resolve->type = Type::promise(made->type);

  return resolve;
}

Expr *ResultAsyncChecker::checkResultAsyncStatic(CallExpr *call, MemberExpr *member,
                                                 const Type *expected) {
  auto *name = dyn_cast<NameExpr>(member->object);
  if (!name || call->optional || member->optional ||
      checker_.resultTypes().neverthrowName(name->name) != "ResultAsync") {
    return nullptr;
  }
  checker_.file_->usesAsync = true;
  llvm::StringRef method = member->name;
  if (method == "fromPromise" || method == "fromSafePromise") {
    return checkResultFromPromise(call, member->loc(), method == "fromSafePromise", expected);
  }
  if (method == "fromThrowable") {
    return checkResultAsyncThrowable(call, member->loc());
  }
  std::string hint;
  if (method == "combine" || method == "combineWithAllErrors") {
    hint = "; await them with 'Promise.all' and use 'Result." + method.str() + "'";
  }
  checker_.diags_.error(member->loc(), "'" + name->name + "." + method +
                                           "' is not supported; ResultAsync has 'fromPromise', "
                                           "'fromSafePromise' and 'fromThrowable'" +
                                           hint);
  for (Expr *&arg : call->args) {
    arg = checker_.exprChecker().checkExpr(arg);
  }

  return call;
}

// fromPromise(p, toError) and fromSafePromise(p): an Ok of what p gives, or
// an Err of what toError makes of its error (fromSafePromise lets the error
// reject the ResultAsync).
Expr *ResultAsyncChecker::checkResultFromPromise(CallExpr *call, llvm::SMLoc loc, bool safe,
                                                 const Type *expected) {
  call->type = Type::error();
  llvm::StringRef fn = safe ? "fromSafePromise" : "fromPromise";
  if (checker_.callChecker().rejectSpread(call) ||
      !checker_.argumentChecker().checkArity(call, loc, safe ? 1 : 2, safe ? 1 : 2)) {
    return call;
  }
  const Type *wanted = wantedResult(expected);
  Expr *&promise = call->args[0];
  const Type *wantedValue = wanted && !wanted->okType()->isNever() ? wanted->okType() : nullptr;
  promise = checker_.exprChecker().checkExpr(
      promise, wantedValue && (isa<NewExpr>(promise) || isa<CallExpr>(promise))
                   ? Type::promise(wantedValue)
                   : nullptr);
  if (promise->type->isError()) {
    return call;
  }
  if (!promise->type->isPromise()) {
    checker_.diags_.error(promise->loc(),
                          "'" + fn + "' takes a Promise, found '" + promise->type->name() + "'");
    return call;
  }
  const Type *value = promise->type->promised(), *error = Type::never();
  if (!safe) {
    Checker::Signature sig;
    sig.params = {checker_.errorClass_->type};
    sig.pad = false;
    call->args[1] = checker_.callbackChecker().checkFunctionArg(call->args[1], sig, 1, fn);
    if (!call->args[1]->type->isFunction()) {
      return call;
    }
    if (auto *fe = dyn_cast<FunctionExpr>(call->args[1])) {
      fe->fn->nonEscaping = false; // it runs later
    }
    error = call->args[1]->type->ret();
    if (error->isVoid()) {
      checker_.diags_.error(call->args[1]->loc(),
                            "the function that turns the Error into the error of the "
                            "Result must return it");
      return call;
    }
  }
  // The halves of the type expected, where they go.
  if (wanted && TypeRelations::isImplicitlyConvertible(value, wanted->okType()) &&
      TypeRelations::isImplicitlyConvertible(error, wanted->errType())) {
    value = wanted->okType();
    error = wanted->errType();
  }
  call->builtin = safe ? Builtin::ResultAsyncFromSafePromise : Builtin::ResultAsyncFromPromise;
  call->type = Type::promise(Type::result(value, error));

  return call;
}

// ResultAsync.fromThrowable(f, toError?): a function with the parameters of
// f, which gives a ResultAsync of what f's Promise gives, or of what f throws
// or its Promise is rejected with, as toError makes it.
Expr *ResultAsyncChecker::checkResultAsyncThrowable(CallExpr *call, llvm::SMLoc loc) {
  call->type = Type::error();
  if (checker_.callChecker().rejectSpread(call) ||
      !checker_.argumentChecker().checkArity(call, loc, 1, 2)) {
    return call;
  }
  Expr *&fn = call->args[0];
  fn = checker_.exprChecker().checkExpr(fn);
  const Type *fnType = fn->type;
  if (fnType->isError()) {
    return call;
  }
  if (!fnType->isFunction() || !fnType->ret()->isPromise()) {
    checker_.diags_.error(fn->loc(),
                          "'fromThrowable' of ResultAsync takes a function that gives a Promise, "
                          "found '" +
                              fnType->name() + "'");
    return call;
  }
  const Type *error = checker_.errorClass_->type;
  if (call->args.size() > 1) {
    Checker::Signature sig;
    sig.params = {checker_.errorClass_->type};
    sig.minArgs = 1;
    Expr *&toError = call->args[1];
    toError = isa<FunctionExpr>(toError)
                  ? checker_.closureChecker().checkFunctionExpr(cast<FunctionExpr>(toError), &sig)
                  : checker_.exprChecker().checkExpr(toError);
    const Type *toType = toError->type;
    if (toType->isError()) {
      return call;
    }
    if (!toType->isFunction() || toType->minArgs() > 1 ||
        (!toType->params().empty() && toType->params()[0] != checker_.errorClass_->type)) {
      checker_.diags_.error(toError->loc(),
                            "the second argument of 'fromThrowable' turns the Error into "
                            "the error of the Result: a function that takes an Error, e.g. "
                            "'(e) => e.message'");
      return call;
    }
    error = toType->ret();
    if (error->isVoid()) {
      checker_.diags_.error(toError->loc(),
                            "the function that turns the Error into the error of the "
                            "Result must return it");
      return call;
    }
  }
  call->builtin = Builtin::ResultAsyncFromThrowable;
  call->type = Type::function(fnType->params(), fnType->minArgs(), fnType->hasRest(),
                              Type::promise(Type::result(fnType->ret()->promised(), error)));

  return call;
}

Expr *ResultAsyncChecker::checkResultAsyncMethod(CallExpr *call, MemberExpr *member,
                                                 llvm::StringRef name) {
  call->type = Type::error();
  checker_.file_->usesAsync = true;
  auto checkArgsLoosely = [&] {
    for (Expr *&arg : call->args) {
      arg = checker_.exprChecker().checkExpr(arg);
    }
  };
  if (checker_.callChecker().rejectSpread(call)) {
    return call;
  }
  // asyncMap and asyncAndThen of a Result: map and andThen of a ResultAsync
  // as to types, but the function runs now and must give a promise.
  llvm::StringRef given = name;
  bool ofResult = member->object->type->isResult();
  if (ofResult) {
    name = name == "asyncMap" ? "map" : "andThen";
  }
  const Type *result = ofResult ? member->object->type : member->object->type->promised();
  const Type *value = result->okType(), *error = result->errType();
  // A callback: what it gets, and what it gives, as `await` would.
  auto callback = [&](Expr *&arg, const Type *half) -> const Type * {
    Checker::Signature sig;
    sig.pad = false;
    sig.params = {halfOf(half)};
    arg = checker_.callbackChecker().checkFunctionArg(arg, sig, 1, given);
    if (!arg->type->isFunction()) {
      return nullptr;
    }
    if (auto *fe = dyn_cast<FunctionExpr>(arg)) {
      fe->fn->nonEscaping = false; // it runs later
    }
    if (ofResult && !arg->type->ret()->isPromise()) {
      checker_.diags_.error(arg->loc(), "the function of '" + given + "' must give a " +
                                            (given == "asyncMap" ? "Promise" : "ResultAsync") +
                                            ", found '" + arg->type->ret()->name() + "'; use '" +
                                            name + "' for a function that gives " +
                                            (given == "asyncMap" ? "a value" : "a Result"));
      return nullptr;
    }

    return awaited(arg->type->ret());
  };
  if (name == "andThrough") {
    checker_.diags_.error(member->loc(), "'andThrough' of a ResultAsync is not supported yet; use "
                                         "'andThen', which gives the Result its function gives");
    if (call->args.size() == 1) {
      callback(call->args[0], value); // for the errors in it
    } else {
      checkArgsLoosely();
    }
    return call;
  }
  struct Method {
    llvm::StringLiteral name;
    Builtin builtin;
    unsigned args;
  };
  static const Method methods[] = {
      {"map", Builtin::ResultAsyncMap, 1},         {"mapErr", Builtin::ResultAsyncMapErr, 1},
      {"andThen", Builtin::ResultAsyncAndThen, 1}, {"orElse", Builtin::ResultAsyncOrElse, 1},
      {"match", Builtin::ResultAsyncMatch, 2},     {"unwrapOr", Builtin::ResultAsyncUnwrapOr, 1},
      {"andTee", Builtin::ResultAsyncAndTee, 1},   {"orTee", Builtin::ResultAsyncOrTee, 1}};
  const Method *method = nullptr;
  for (const Method &m : methods) {
    if (m.name == name) {
      method = &m;
    }
  }
  if (!method ||
      !checker_.argumentChecker().checkArity(call, member->loc(), method->args, method->args)) {
    return call;
  }
  call->builtin = method->builtin;
  const Type *type = nullptr;
  switch (method->builtin) {
  case Builtin::ResultAsyncMap:
  case Builtin::ResultAsyncMapErr: {
    bool onError = method->builtin == Builtin::ResultAsyncMapErr;
    const Type *made = callback(call->args[0], onError ? error : value);
    if (!made) {
      return call;
    }
    type = onError ? Type::result(value, made) : Type::result(made, error);
    break;
  }
  case Builtin::ResultAsyncAndThen:
  case Builtin::ResultAsyncOrElse: {
    bool onError = method->builtin == Builtin::ResultAsyncOrElse;
    const Type *made = callback(call->args[0], onError ? error : value);
    if (!made) {
      return call;
    }
    if (!made->isResult()) {
      checker_.diags_.error(call->args[0]->loc(),
                            "the function of '" + given + "' must give a " +
                                (ofResult ? "ResultAsync" : "Result or a ResultAsync") +
                                ", found '" + call->args[0]->type->ret()->name() +
                                "'; to give a value, use '" +
                                (onError    ? "mapErr"
                                 : ofResult ? "asyncMap"
                                            : "map") +
                                "'");
      return call;
    }
    // The halves the other way meet those of the Result the function gives.
    const Type *kept = onError ? made->errType() : made->okType();
    const Type *mine = onError ? value : error;
    const Type *theirs = onError ? made->okType() : made->errType();
    const Type *other = ResultTypes::unifyHalf(mine, theirs);
    if (!other) {
      checker_.diags_.error(call->args[0]->loc(),
                            std::string("the ") + (onError ? "values" : "errors") + " '" +
                                mine->name().str() + "' and '" + theirs->name().str() +
                                "' do not go into one type; make them one, e.g. with '" +
                                (onError ? "map" : "mapErr") + "'");
      return call;
    }
    type = onError ? Type::result(other, kept) : Type::result(kept, other);
    break;
  }
  case Builtin::ResultAsyncMatch: {
    const Type *a = callback(call->args[0], value), *b = callback(call->args[1], error);
    if (!a || !b) {
      return call;
    }
    bool voids = a->isVoid() && b->isVoid();
    const Type *both = voids                        ? a
                       : a->isVoid() || b->isVoid() ? nullptr
                                                    : TypeRelations::unify(a, b);
    if (!both || both->isError()) {
      checker_.diags_.error(call->loc(), "'match' gives '" + a->name() + "' for an Ok and '" +
                                             b->name() + "' for an Err, and no type holds both");
      return call;
    }
    call->type = Type::promise(both);
    return call;
  }
  case Builtin::ResultAsyncUnwrapOr: {
    Expr *&arg = call->args[0];
    arg = checker_.exprChecker().checkExpr(arg,
                                           !value->isNever() && !value->isVoid() ? value : nullptr);
    const Type *both = arg->type->isError() ? nullptr : TypeRelations::unify(value, arg->type);
    if (!both || both->isError()) {
      if (!arg->type->isError()) {
        checker_.diags_.error(arg->loc(), "'unwrapOr' gives '" + value->name() +
                                              "' for an Ok and '" + arg->type->name() +
                                              "' otherwise, and no type holds both");
      }
      return call;
    }
    arg = checker_.typeRelations().convert(arg, both);
    call->type = Type::promise(both);
    return call;
  }
  default: // andTee and orTee: what the function gives is dropped
    if (!callback(call->args[0], method->builtin == Builtin::ResultAsyncOrTee ? error : value)) {
      return call;
    }
    type = result;
    break;
  }
  call->type = Type::promise(type);

  return call;
}

} // namespace tz
