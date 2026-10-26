#include "compiler/check/async/promise_checker/promise_checker.h"
#include "compiler/check/expr/argument_checker/argument_checker.h"
#include "compiler/check/expr/call_checker/call_checker.h"
#include "compiler/check/expr/expr_checker/expr_checker.h"
#include "compiler/check/flow/flow_checker/flow_checker.h"
#include "compiler/check/functions/callback_checker/callback_checker.h"
#include "compiler/check/functions/closure_checker/closure_checker.h"
#include "compiler/check/types/destination/destination.h"
#include "compiler/check/types/literal_types/literal_types.h"
#include "compiler/check/types/type_relations/type_relations.h"
#include "compiler/check/types/type_resolver/type_resolver.h"
#include "compiler/infer/return_inferrer/return_inferrer.h"
#include "compiler/library/result/result_checker/result_checker.h"
#include "compiler/whole_program/flows/flows.h"
#include "llvm/ADT/STLExtras.h"

using llvm::cast;
using llvm::dyn_cast;
using llvm::dyn_cast_or_null;
using llvm::isa;

namespace tz {

namespace {

const Type *awaited(const Type *type) { return type->isPromise() ? type->promised() : type; }

}

PromiseChecker::PromiseChecker(Checker &checker) : checker_(checker) {}

const Type *PromiseChecker::settledResultType(const Type *value) {
  std::vector<Field> fields{{"status", Type::string(), false}};
  if (!value->isVoid() && !value->isNever()) {
    fields.push_back({"value", Type::optional(value), false});
  }
  fields.push_back({"reason", Type::optional(checker_.errorClass_->type), false});

  return Type::object(std::move(fields), "PromiseSettledResult<" + value->name().str() + ">");
}

const Type *PromiseChecker::resolverType(const Type *value) {
  std::vector<const Type *> params;
  if (!value->isVoid()) {
    params.push_back(value);
  }

  return Type::function(params, params.size(), false, Type::voidTy());
}

const Type *PromiseChecker::rejecterType() {
  return Type::function({checker_.errorClass_->type}, 1, false, Type::voidTy());
}

const Type *PromiseChecker::executorValueType(const Expr *executor) {
  auto *fe = dyn_cast<FunctionExpr>(executor);
  if (!fe) {
    return nullptr;
  }
  if (fe->fn->params.empty()) {
    return Type::voidTy();
  }
  llvm::StringRef resolve = fe->fn->params[0].name;
  bool given = false;
  visitExprs(fe->fn->body, [&](const Expr *e) {
    auto *call = dyn_cast<CallExpr>(e);
    auto *name = call ? dyn_cast<NameExpr>(call->callee) : nullptr;
    given |= name && name->name == resolve && !call->args.empty();
  });
  if (!given) {
    return Type::voidTy();
  }
  if (fe->fn->source.empty()) {
    return nullptr;
  }
  FunctionDecl *copy = checker_.returnInferrer().freshCopy(fe->fn);
  copy->sym = nullptr;
  auto *trial = checker_.ctx_.make<FunctionExpr>(fe->loc(), copy);
  Checker::Flow saved = checker_.flow_;
  bool muted = checker_.diags_.setMuted(true);
  Checker::Signature sig;
  sig.params = {Type::error(), Type::error()};
  sig.ret = Type::voidTy();
  sig.voidResult = true;
  sig.pad = false;
  checker_.closureChecker().checkFunctionExpr(trial, &sig);
  checker_.diags_.setMuted(muted);
  checker_.flow_ = std::move(saved);
  const Type *found = nullptr;
  bool unknown = false;
  visitExprs(copy->body, [&](const Expr *e) {
    auto *call = dyn_cast<CallExpr>(e);
    auto *name = call ? dyn_cast<NameExpr>(call->callee) : nullptr;
    if (!name || name->name != resolve || call->args.empty()) {
      return;
    }
    const Type *t = call->args[0]->type;
    if (!t || t->isError() || t->isVoid() || t->isUndefined()) {
      unknown = true;
      return;
    }
    const Type *common = found ? LiteralTypes::unionUnify(found, t) : t;
    if (!common || common->isError()) {
      unknown = true;
    } else {
      found = common;
    }
  });

  return unknown ? nullptr : found;
}

Expr *PromiseChecker::checkPromiseNew(NewExpr *expr, const Type *expected) {
  checker_.file_->usesAsync = true;
  auto *call = checker_.ctx_.make<CallExpr>(expr->loc(), expr, expr->args);
  call->builtin = Builtin::PromiseNew;
  call->type = Type::error();
  const Type *value = nullptr;
  if (expr->typeArgs.size() > 1) {
    checker_.diags_.error(expr->loc(), "'Promise' takes one type argument, the type of its value");
    value = Type::error();
  } else if (expr->typeArgs.size() == 1) {
    value = awaited(checker_.typeResolver().resolveType(expr->typeArgs[0], true));
  } else if (expected && expected->nonOptional()->isPromise() &&
             !expected->nonOptional()->promised()->isNever()) {
    value = expected->nonOptional()->promised();
  } else if (call->args.size() == 1) {
    value = executorValueType(call->args[0]);
    if (!value) {
      checker_.diags_.error(expr->loc(),
                            "cannot tell the type of the Promise's value from what is given "
                            "to 'resolve'; write it, e.g. 'new Promise<int>(...)'");
      value = Type::error();
    }
  }
  if (!checker_.argumentChecker().checkArity(call, expr->loc(), 1, 1)) {
    return call;
  }
  if (value->isError()) {
    Checker::Signature loose;
    loose.params = {Type::error(), Type::error()};
    loose.pad = false;
    call->args[0] =
        isa<FunctionExpr>(call->args[0])
            ? checker_.closureChecker().checkFunctionExpr(cast<FunctionExpr>(call->args[0]), &loose)
            : checker_.exprChecker().checkExpr(call->args[0]);
    return call;
  }
  Checker::Signature sig;
  sig.params = {resolverType(value), rejecterType()};
  sig.minArgs = 2;
  sig.ret = Type::voidTy();
  sig.voidResult = true;
  sig.pad = false;
  call->args[0] = checker_.callbackChecker().checkFunctionArg(call->args[0], sig, 2, "new Promise");
  checker_.flowChecker().calledFunction();
  call->type = Type::promise(value);

  return call;
}

Expr *PromiseChecker::checkPromiseMethod(CallExpr *call, MemberExpr *member, const Type *expected) {
  const Type *type = member->object->type;
  const Type *value = type->promised();
  llvm::StringRef name = member->name;
  static const llvm::StringLiteral resultMethods[] = {"isOk",    "isErr",  "map",       "mapErr",
                                                      "andThen", "orElse", "match",     "unwrapOr",
                                                      "andTee",  "orTee",  "andThrough"};
  auto checkArgsLoosely = [&] {
    for (Expr *&arg : call->args) {
      arg = checker_.exprChecker().checkExpr(arg);
    }
  };
  if (value->isResult() && llvm::is_contained(resultMethods, name)) {
    auto *object = dyn_cast<NameExpr>(member->object);
    std::string promise = object ? object->name.str() : "p";
    return checker_.resultChecker().reject(
        call, member->loc(),
        "'" + name.str() +
            "' is a method of Result, not of Promise; await the Promise first: "
            "'(await " +
            promise + ")." + name.str() + "(...)'");
  }
  if (checker_.callChecker().rejectSpread(call)) {
    return call;
  }
  checker_.file_->usesAsync = true;
  const Type *wanted = expected && expected->nonOptional()->isPromise()
                           ? expected->nonOptional()->promised()
                           : nullptr;
  bool failed = false;
  auto callback = [&](Expr *&arg, const Type *param) -> const Type * {
    if (isa<UndefinedExpr>(arg)) {
      arg = checker_.exprChecker().checkExpr(arg);
      return nullptr;
    }
    Checker::Signature sig;
    sig.pad = false;
    if (param && !param->isVoid()) {
      sig.params = {param};
    }
    sig.minArgs = 0;
    if (wanted && isa<FunctionExpr>(arg) && Destination::literalShaped(arg)) {
      sig.ret = wanted;
    }
    arg = checker_.callbackChecker().checkFunctionArg(arg, sig, 1, name);
    if (!arg->type->isFunction()) {
      failed = true;
      return nullptr;
    }
    if (auto *fe = dyn_cast<FunctionExpr>(arg)) {
      fe->fn->nonEscaping = false;
    }

    return awaited(arg->type->ret());
  };
  auto both = [&](const Type *a, const Type *b, llvm::SMLoc loc) -> const Type * {
    if (a->isVoid() && b->isVoid()) {
      return a;
    }
    if (a->isVoid() || a->isUndefined()) {
      return b->isVoid() ? b : Type::optional(b);
    }
    if (b->isVoid() || b->isUndefined()) {
      return Type::optional(a);
    }
    const Type *common = TypeRelations::unify(a, b);
    if (!common || common->isError()) {
      checker_.diags_.error(loc,
                            "'" + name + "' gives '" + a->name() + "' one way and '" + b->name() +
                                "' the other, and no type holds both; make them give one type");
      return Type::error();
    }

    return common;
  };

  const Type *result = nullptr;
  if (name == "then") {
    if (!checker_.argumentChecker().checkArity(call, member->loc(), 1, 2)) {
      return call;
    }
    call->builtin = Builtin::PromiseThen;
    const Type *onFulfilled = callback(call->args[0], value);
    const Type *onRejected =
        call->args.size() > 1 ? callback(call->args[1], checker_.errorClass_->type) : nullptr;
    if (failed) {
      return call;
    }
    const Type *fulfilled = onFulfilled ? onFulfilled : value;
    result = onRejected ? both(fulfilled, onRejected, call->loc()) : fulfilled;
  } else if (name == "catch") {
    if (!checker_.argumentChecker().checkArity(call, member->loc(), 1, 1)) {
      return call;
    }
    call->builtin = Builtin::PromiseCatch;
    const Type *onRejected = callback(call->args[0], checker_.errorClass_->type);
    if (failed) {
      return call;
    }
    result = onRejected ? both(value, onRejected, call->loc()) : value;
  } else if (name == "finally") {
    if (!checker_.argumentChecker().checkArity(call, member->loc(), 1, 1)) {
      return call;
    }
    call->builtin = Builtin::PromiseFinally;
    callback(call->args[0], nullptr);
    if (failed) {
      return call;
    }
    result = value;
  } else {
    checker_.diags_.error(member->loc(), "property '" + name + "' does not exist on type '" +
                                             type->name() +
                                             "'; a Promise has 'then', 'catch' and "
                                             "'finally'");
    checkArgsLoosely();
    return call;
  }
  if (result->isError()) {
    return call;
  }
  if (result->isNever()) {
    result = value;
  }
  call->type = Type::promise(result);

  return call;
}

}
