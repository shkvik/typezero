#include "compiler/pipeline/check_program/topics/result/result_method_checker/result_method_checker.h"
#include "compiler/pipeline/check_program/flow/flow_checker/flow_checker.h"
#include "compiler/pipeline/check_program/topics/expr/argument_checker/argument_checker.h"
#include "compiler/pipeline/check_program/topics/expr/call_checker/call_checker.h"
#include "compiler/pipeline/check_program/topics/expr/expr_checker/expr_checker.h"
#include "compiler/pipeline/check_program/topics/functions/callback_checker/callback_checker.h"
#include "compiler/pipeline/check_program/topics/result/result_checker/result_checker.h"
#include "compiler/pipeline/check_program/topics/result/result_types/result_types.h"
#include "compiler/pipeline/check_program/types/literal_types/literal_types.h"
#include "compiler/pipeline/check_program/types/type_relations/type_relations.h"
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

ResultMethodChecker::ResultMethodChecker(CheckState &owner)
    : state(owner) {}

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
      arg = ExprChecker(this->state).checkExpr(arg);
    }
  };
  if (CallChecker(this->state).rejectSpread(call)) {
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
    std::string message = ErrorTextResultMethod::noMember(name, type->name());
    this->state.getDiagnostics().error(member->loc(), message);
    checkArgsLoosely();
    return call;
  }
  ArgumentChecker::ResultArityMatches arity =
      ArgumentChecker(this->state).checkArity(call, method->args, method->args);
  if (arity.isErr()) {
    this->state.getDiagnostics().error(member->loc(), arity.error().message);
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
    arg = ExprChecker(this->state)
              .checkExpr(arg, ResultMethodChecker::givesType(value) ? value : nullptr);
    const Type *other = arg->type;
    if (other->isError()) {
      return call;
    }
    const Type *result = other->isVoid() ? nullptr : LiteralTypes::unionUnify(value, other);
    if (!result || result->isError()) {
      std::string message = ErrorTextResultMethod::unwrapOr(value->name(), other->name());
      this->state.getDiagnostics().error(arg->loc(), message);
      return call;
    }
    arg = TypeRelations(this->state).convert(arg, result);
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
      arg = ExprChecker(this->state).checkExpr(arg);
      if (arg->type->isFunction()) {
        return arg->type->ret();
      }
      if (!arg->type->isError()) {
        std::string message = ErrorTextResultMethod::function(name, arg->type->name());
        this->state.getDiagnostics().error(arg->loc(), message);
      }
      return nullptr;
    }
    Signature sig;
    sig.pad = false;
    sig.params = {ResultMethodChecker::halfParam(half)};
    sig.voidResult = builtin == Builtin::ResultAndTee || builtin == Builtin::ResultOrTee;
    if (isa<FunctionExpr>(arg) && !sig.voidResult) {
      sig.ret = ret;
    }
    arg = CallbackChecker(this->state).checkFunctionArg(arg, sig, 1, name);

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
                half && (half->holdsObjects() || half->holdsResults()) ? half : nullptr);
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
    FlowChecker(this->state).calledFunction();
    if (!a || !b) {
      return call;
    }
    if (a->isVoid() && b->isVoid()) {
      call->type = a;
      return call;
    }
    const Type *result = a->isVoid() || b->isVoid() ? nullptr : LiteralTypes::unionUnify(a, b);
    if (!result || result->isError()) {
      std::string message = ErrorTextResultMethod::match(a->name(), b->name());
      this->state.getDiagnostics().error(member->loc(), message);
      return call;
    }
    call->type = result;
    return call;
  }
  default:
    ret = check(call->args[0], onError ? error : value, nullptr);
    break;
  }
  FlowChecker(this->state).calledFunction();
  if (!ret) {
    return call;
  }

  auto needsResult = [&]() {
    if (ret->isResult()) {
      return true;
    }
    bool andThen = builtin == Builtin::ResultAndThen;
    std::string message = ErrorTextResultMethod::result(name, ret->name(), andThen);
    this->state.getDiagnostics().error(call->args[0]->loc(), message);

    return false;
  };
  auto oneError = [&](const Type *a, const Type *b) {
    const Type *common = ResultTypes::unifyHalf(a, b);
    if (!common) {
      std::string message = ErrorTextResultMethod::errors(a, name, b);
      this->state.getDiagnostics().error(call->args[0]->loc(), message);
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
      std::string message = ErrorTextResultMethod::values(value, ret->okType());
      this->state.getDiagnostics().error(call->args[0]->loc(), message);
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
