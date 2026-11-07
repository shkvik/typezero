#include "compiler/library/result/result_method_checker/result_method_checker.h"
#include "compiler/check/expr/argument_checker/argument_checker.h"
#include "compiler/check/expr/call_checker/call_checker.h"
#include "compiler/check/expr/expr_checker/expr_checker.h"
#include "compiler/check/flow/flow_checker/flow_checker.h"
#include "compiler/check/functions/callback_checker/callback_checker.h"
#include "compiler/check/types/literal_types/literal_types.h"
#include "compiler/check/types/type_relations/type_relations.h"
#include "compiler/library/result/result_checker/result_checker.h"
#include "compiler/library/result/result_types/result_types.h"
#include "llvm/ADT/STLExtras.h"

using llvm::dyn_cast;
using llvm::isa;

namespace tz {

const Type *ResultMethodChecker::halfParam(const Type *half) {
  return half->isVoid() ? Type::undefinedTy() : half;
}

bool ResultMethodChecker::givesType(const Type *half) {
  return !half->isNever() && !half->isVoid();
}

static std::string noUnion(const Type *a, const Type *b) {
  if (a->isVoid() || b->isVoid()) {
    return ": one of them is nothing, 'ok()'";
  }
  std::string problem = unionProblem(Type::unionOf({a, b}));

  return problem.empty() ? "" : ": " + problem;
}

ResultMethodChecker::ResultMethodChecker(Checker &owner)
    : checker(owner) {}

Expr *ResultMethodChecker::checkResultMethod(CallExpr *call, MemberExpr *member,
                                             const Type *expected) {
  const Type *type = member->object->type;
  const Type *value = type->okType();
  const Type *error = type->errType();
  if (auto *conv = dyn_cast<CastExpr>(member->object);
      conv && conv->isImplicit() && conv->operand->type->isResult()) {
    if (value->isNever()) {
      value = conv->operand->type->okType();
    }
    if (error->isNever()) {
      error = conv->operand->type->errType();
    }
  }
  llvm::StringRef name = member->name;
  auto checkArgsLoosely = [&] {
    for (Expr *&arg : call->args) {
      arg = this->checker.exprChecker().checkExpr(arg);
    }
  };
  if (this->checker.callChecker().rejectSpread(call)) {
    return call;
  }
  struct Method {
    llvm::StringLiteral name;
    Builtin builtin;
    unsigned args;
  };
  static const Method methods[] = {
      {"isOk", Builtin::ResultIsOk, 0},
      {"isErr", Builtin::ResultIsErr, 0},
      {"map", Builtin::ResultMap, 1},
      {"mapErr", Builtin::ResultMapErr, 1},
      {"andThen", Builtin::ResultAndThen, 1},
      {"orElse", Builtin::ResultOrElse, 1},
      {"match", Builtin::ResultMatch, 2},
      {"unwrapOr", Builtin::ResultUnwrapOr, 1},
      {"andTee", Builtin::ResultAndTee, 1},
      {"orTee", Builtin::ResultOrTee, 1},
      {"andThrough", Builtin::ResultAndThrough, 1},
  };
  const Method *method = nullptr;
  for (const Method &m : methods) {
    if (m.name == name) {
      method = &m;
    }
  }
  if (!method) {
    this->checker.diags_.error(member->loc(), "property '" + name + "' does not exist on type '" +
                                                  type->name() + "'");
    checkArgsLoosely();
    return call;
  }
  ArgumentChecker::ResultArityMatches arity =
      this->checker.argumentChecker().checkArity(call, method->args, method->args);
  if (arity.isErr()) {
    this->checker.diags_.error(member->loc(), arity.error().message);
    return call;
  }
  Builtin builtin = method->builtin;
  call->target = BuiltinCall{.builtin = builtin};

  if (builtin == Builtin::ResultIsOk || builtin == Builtin::ResultIsErr) {
    call->type = Type::boolTy();
    return call;
  }

  if (builtin == Builtin::ResultUnwrapOr) {
    Expr *&arg = call->args[0];
    arg = this->checker.exprChecker().checkExpr(
        arg, ResultMethodChecker::givesType(value) ? value : nullptr);
    const Type *other = arg->type;
    if (other->isError()) {
      return call;
    }
    const Type *result = other->isVoid() ? nullptr : LiteralTypes::unionUnify(value, other);
    if (!result || result->isError()) {
      this->checker.diags_.error(
          arg->loc(), "'unwrapOr' gives '" + value->name() + "' for an Ok and '" + other->name() +
                          "' otherwise, and no type holds both; say what each gives with 'match'");
      return call;
    }
    arg = this->checker.typeRelations().convert(arg, result);
    call->type = result;
    return call;
  }

  bool onError = builtin == Builtin::ResultMapErr || builtin == Builtin::ResultOrElse ||
                 builtin == Builtin::ResultOrTee;
  const Type *wanted = expected ? expected->nonOptional() : nullptr;
  if (wanted && wanted->isError()) {
    wanted = nullptr;
  }
  auto check = [&](Expr *&arg, const Type *half, const Type *ret) -> const Type * {
    if (half->isNever() && !isa<FunctionExpr>(arg)) {
      arg = this->checker.exprChecker().checkExpr(arg);
      if (arg->type->isFunction()) {
        return arg->type->ret();
      }
      if (!arg->type->isError()) {
        this->checker.diags_.error(arg->loc(), "'" + name + "' needs a function, found '" +
                                                   arg->type->name() + "'");
      }
      return nullptr;
    }
    Checker::Signature sig;
    sig.pad = false;
    sig.params = {ResultMethodChecker::halfParam(half)};
    sig.voidResult = builtin == Builtin::ResultAndTee || builtin == Builtin::ResultOrTee;
    if (isa<FunctionExpr>(arg) && !sig.voidResult) {
      sig.ret = ret;
    }
    arg = this->checker.callbackChecker().checkFunctionArg(arg, sig, 1, name);

    return arg->type->isFunction() ? arg->type->ret() : nullptr;
  };
  const Type *wantedResult = wanted && wanted->isResult() ? wanted : nullptr;
  const Type *ret = nullptr;
  switch (builtin) {
  case Builtin::ResultMap:
  case Builtin::ResultMapErr: {
    const Type *half =
        wantedResult ? (onError ? wantedResult->errType() : wantedResult->okType()) : nullptr;
    ret = check(call->args[0], onError ? error : value,
                half && (Checker::holdsObjects(half) || Checker::holdsResults(half)) ? half
                                                                                     : nullptr);
    break;
  }
  case Builtin::ResultAndThen:
  case Builtin::ResultOrElse:
    ret = check(call->args[0], onError ? error : value, wantedResult);
    break;
  case Builtin::ResultMatch: {
    const Type *both = wanted && !wanted->isVoid() ? wanted : nullptr;
    const Type *a = check(call->args[0], value, both);
    const Type *b = check(call->args[1], error, both);
    this->checker.flowChecker().calledFunction();
    if (!a || !b) {
      return call;
    }
    if (a->isVoid() && b->isVoid()) {
      call->type = a;
      return call;
    }
    const Type *result = a->isVoid() || b->isVoid() ? nullptr : LiteralTypes::unionUnify(a, b);
    if (!result || result->isError()) {
      this->checker.diags_.error(member->loc(), "the functions given to 'match' return '" +
                                                    a->name() + "' and '" + b->name() +
                                                    "', and no type holds both");
      return call;
    }
    call->type = result;
    return call;
  }
  default:
    ret = check(call->args[0], onError ? error : value, nullptr);
    break;
  }
  this->checker.flowChecker().calledFunction();
  if (!ret) {
    return call;
  }

  auto needsResult = [&]() {
    if (ret->isResult()) {
      return true;
    }
    this->checker.diags_.error(
        call->args[0]->loc(),
        "the function given to '" + name +
            "' must return a Result, 'ok(...)' or 'err(...)', found '" + ret->name() + "'" +
            (builtin == Builtin::ResultAndThen ? "; to change the value, use 'map'" : ""));

    return false;
  };
  auto oneError = [&](const Type *a, const Type *b) {
    const Type *common = ResultTypes::unifyHalf(a, b);
    if (!common) {
      this->checker.diags_.error(
          call->args[0]->loc(),
          "the Result has errors of '" + a->name() + "' and the function given to '" + name +
              "' returns errors of '" + b->name() + "', which no union holds" + noUnion(a, b) +
              "; convert one with 'mapErr'");
    }

    return common;
  };
  switch (builtin) {
  case Builtin::ResultMap:
    call->type = Type::result(ret, error);
    break;
  case Builtin::ResultMapErr:
    call->type = Type::result(value, ret);
    break;
  case Builtin::ResultAndThen:
    if (!needsResult()) {
      return call;
    }
    if (const Type *common = oneError(error, ret->errType())) {
      call->type = Type::result(ret->okType(), common);
    }
    break;
  case Builtin::ResultAndThrough:
    if (!needsResult()) {
      return call;
    }
    if (const Type *common = oneError(error, ret->errType())) {
      call->type = Type::result(value, common);
    }
    break;
  case Builtin::ResultOrElse: {
    if (!needsResult()) {
      return call;
    }
    const Type *common = ResultTypes::unifyHalf(value, ret->okType());
    if (!common) {
      this->checker.diags_.error(call->args[0]->loc(),
                                 "the Result has values of '" + value->name() +
                                     "' and the function given to 'orElse' returns values of '" +
                                     ret->okType()->name() + "', which no union holds" +
                                     noUnion(value, ret->okType()));
      return call;
    }
    call->type = Type::result(common, ret->errType());
    break;
  }
  default:
    call->type = type;
    break;
  }

  return call;
}

}
