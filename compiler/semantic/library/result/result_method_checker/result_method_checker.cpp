#include "compiler/semantic/library/result/result_method_checker/result_method_checker.h"
#include "compiler/program/library/result/result_method_table/result_method_table.h"
#include "compiler/semantic/bodies/expr/argument_checker/argument_checker.h"
#include "compiler/semantic/bodies/expr/call_checker/call_checker.h"
#include "compiler/semantic/bodies/expr/expr_checker/expr_checker.h"
#include "compiler/semantic/bodies/functions/callback_checker/callback_checker.h"
#include "compiler/semantic/flow/flow_checker/flow_checker.h"
#include "compiler/semantic/library/result/result_checker/result_checker.h"
#include "compiler/semantic/library/result/result_types/result_types.h"
#include "compiler/semantic/semantic_parts/semantic_parts.h"
#include "compiler/semantic/types/literal_types/literal_types.h"
#include "compiler/semantic/types/type_relations/type_relations.h"
#include "llvm/ADT/STLExtras.h"

using llvm::dyn_cast;
using llvm::isa;

namespace tz {

ResultMethodChecker::ResultMethodChecker(SemanticState &owner, SemanticParts &all)
    : state(owner) {
  this->callChecker = &all.callChecker;
  this->exprChecker = &all.exprChecker;
  this->flowChecker = &all.flowChecker;
  this->typeRelations = &all.typeRelations;
  this->argumentChecker = &all.argumentChecker;
  this->callbackChecker = &all.callbackChecker;
}

const Type *ResultMethodChecker::halfParam(const Type *half) {
  return half->isVoid() ? Type::undefinedTy() : half;
}

bool ResultMethodChecker::givesType(const Type *half) {
  return !half->isNever() && !half->isVoid();
}

Expr *ResultMethodChecker::checkResultMethod(CallExpr *call, MemberExpr *member,
                                             const Type *expected) {
  ResultCall run = ResultMethodChecker::resultCall(call, member);
  if (this->callChecker->rejectSpread(call)) {
    return call;
  }
  const ResultMethodInfo *method = ResultMethodTable::find(run.name);
  if (method == nullptr) {
    std::string message = ErrorTextResultMethod::noMember(run.name, run.type->name());
    this->state.getDiagnostics().error(member->loc(), message);
    this->walkArgsPlainly(call);
    return call;
  }
  int args = ResultMethodTable::countArgs(*method);
  ArgumentChecker::ResultArityMatches arity = this->argumentChecker->checkArity(call, args, args);
  if (arity.isErr()) {
    this->state.getDiagnostics().error(member->loc(), arity.error().message);
    return call;
  }
  run.builtin = method->builtin;
  call->target = BuiltinCall{.builtin = run.builtin};
  if (run.builtin == Builtin::ResultIsOk || run.builtin == Builtin::ResultIsErr) {
    call->type = Type::boolTy();
    return call;
  }
  if (run.builtin == Builtin::ResultUnwrapOr) {
    return this->checkUnwrapOr(run);
  }
  if (run.builtin == Builtin::ResultMatch) {
    return this->checkResultMatch(run, expected);
  }
  const Type *ret = this->callbackReturn(run, expected);
  this->flowChecker->calledFunction();
  if (ret != nullptr) {
    this->typeResultCall(run, ret);
  }

  return call;
}

ResultMethodChecker::ResultCall ResultMethodChecker::resultCall(CallExpr *call,
                                                                MemberExpr *member) {
  const Type *type = member->object->type;
  const Type *value = type->okType();
  const Type *error = type->errType();
  auto *conv = dyn_cast<CastExpr>(member->object);
  if (conv != nullptr && conv->isImplicit() && conv->operand->type->isResult()) {
    if (value->isNever()) {
      value = conv->operand->type->okType();
    }
    if (error->isNever()) {
      error = conv->operand->type->errType();
    }
  }

  return ResultCall{
      .call = call,
      .member = member,
      .type = type,
      .value = value,
      .error = error,
      .builtin = Builtin::None,
      .name = member->name,
  };
}

void ResultMethodChecker::walkArgsPlainly(CallExpr *call) {
  for (Expr *&arg : call->args) {
    arg = this->exprChecker->checkExpr(arg);
  }
}

const Type *ResultMethodChecker::wantedType(const Type *expected) {
  const Type *wanted = expected != nullptr ? expected->nonOptional() : nullptr;
  if (wanted != nullptr && wanted->isError()) {
    return nullptr;
  }

  return wanted;
}

Expr *ResultMethodChecker::checkUnwrapOr(const ResultCall &run) {
  CallExpr *call = run.call;
  Expr *&arg = call->args[0];
  const Type *hint = ResultMethodChecker::givesType(run.value) ? run.value : nullptr;
  arg = this->exprChecker->checkExpr(arg, hint);
  const Type *other = arg->type;
  if (other->isError()) {
    return call;
  }
  const Type *result = other->isVoid() ? nullptr : LiteralTypes::unionUnify(run.value, other);
  if (result == nullptr || result->isError()) {
    std::string message = ErrorTextResultMethod::unwrapOr(run.value->name(), other->name());
    this->state.getDiagnostics().error(arg->loc(), message);
    return call;
  }
  arg = this->typeRelations->convert(arg, result);
  call->type = result;

  return call;
}

const Type *ResultMethodChecker::halfCallbackReturn(const ResultCall &run, Expr *&arg,
                                                    const Type *half, const Type *ret) {
  if (half->isNever() && !isa<FunctionExpr>(arg)) {
    arg = this->exprChecker->checkExpr(arg);
    if (arg->type->isFunction()) {
      return arg->type->ret();
    }
    if (!arg->type->isError()) {
      std::string message = ErrorTextResultMethod::function(run.name, arg->type->name());
      this->state.getDiagnostics().error(arg->loc(), message);
    }
    return nullptr;
  }
  Signature sig;
  sig.pad = false;
  sig.params = {ResultMethodChecker::halfParam(half)};
  sig.voidResult = run.builtin == Builtin::ResultAndTee || run.builtin == Builtin::ResultOrTee;
  if (isa<FunctionExpr>(arg) && !sig.voidResult) {
    sig.ret = ret;
  }
  arg = this->callbackChecker->checkFunctionArg(arg, sig, 1, run.name);

  return arg->type->isFunction() ? arg->type->ret() : nullptr;
}

Expr *ResultMethodChecker::checkResultMatch(const ResultCall &run, const Type *expected) {
  CallExpr *call = run.call;
  const Type *wanted = ResultMethodChecker::wantedType(expected);
  const Type *both = wanted != nullptr && !wanted->isVoid() ? wanted : nullptr;
  const Type *a = this->halfCallbackReturn(run, call->args[0], run.value, both);
  const Type *b = this->halfCallbackReturn(run, call->args[1], run.error, both);
  this->flowChecker->calledFunction();
  if (a == nullptr || b == nullptr) {
    return call;
  }
  if (a->isVoid() && b->isVoid()) {
    call->type = a;
    return call;
  }
  const Type *result = a->isVoid() || b->isVoid() ? nullptr : LiteralTypes::unionUnify(a, b);
  if (result == nullptr || result->isError()) {
    std::string message = ErrorTextResultMethod::match(a->name(), b->name());
    this->state.getDiagnostics().error(run.member->loc(), message);
    return call;
  }
  call->type = result;

  return call;
}

const Type *ResultMethodChecker::callbackReturn(const ResultCall &run, const Type *expected) {
  Builtin builtin = run.builtin;
  bool onError = builtin == Builtin::ResultMapErr || builtin == Builtin::ResultOrElse ||
                 builtin == Builtin::ResultOrTee;
  const Type *wanted = ResultMethodChecker::wantedType(expected);
  const Type *wantedResult = wanted != nullptr && wanted->isResult() ? wanted : nullptr;
  const Type *half = onError ? run.error : run.value;
  Expr *&arg = run.call->args[0];
  if (builtin == Builtin::ResultMap || builtin == Builtin::ResultMapErr) {
    const Type *wantedHalf = nullptr;
    if (wantedResult != nullptr) {
      wantedHalf = onError ? wantedResult->errType() : wantedResult->okType();
    }
    bool shaped =
        wantedHalf != nullptr && (wantedHalf->holdsObjects() || wantedHalf->holdsResults());
    return this->halfCallbackReturn(run, arg, half, shaped ? wantedHalf : nullptr);
  }
  if (builtin == Builtin::ResultAndThen || builtin == Builtin::ResultOrElse) {
    return this->halfCallbackReturn(run, arg, half, wantedResult);
  }

  return this->halfCallbackReturn(run, arg, half, nullptr);
}

void ResultMethodChecker::typeResultCall(const ResultCall &run, const Type *ret) {
  Builtin builtin = run.builtin;
  if (builtin == Builtin::ResultMap) {
    run.call->type = Type::result(ret, run.error);
    return;
  }
  if (builtin == Builtin::ResultMapErr) {
    run.call->type = Type::result(run.value, ret);
    return;
  }
  if (builtin == Builtin::ResultAndThen || builtin == Builtin::ResultAndThrough) {
    this->typeChainedCall(run, ret);
    return;
  }
  if (builtin == Builtin::ResultOrElse) {
    this->typeOrElseCall(run, ret);
    return;
  }
  run.call->type = run.type;
}

bool ResultMethodChecker::returnsResult(const ResultCall &run, const Type *ret) {
  if (ret->isResult()) {
    return true;
  }
  bool andThen = run.builtin == Builtin::ResultAndThen;
  std::string message = ErrorTextResultMethod::result(run.name, ret->name(), andThen);
  this->state.getDiagnostics().error(run.call->args[0]->loc(), message);

  return false;
}

void ResultMethodChecker::typeChainedCall(const ResultCall &run, const Type *ret) {
  if (!this->returnsResult(run, ret)) {
    return;
  }
  const Type *common = ResultTypes::unifyHalf(run.error, ret->errType());
  if (common == nullptr) {
    std::string message = ErrorTextResultMethod::errors(run.error, run.name, ret->errType());
    this->state.getDiagnostics().error(run.call->args[0]->loc(), message);
    return;
  }
  const Type *okType = run.builtin == Builtin::ResultAndThen ? ret->okType() : run.value;
  run.call->type = Type::result(okType, common);
}

void ResultMethodChecker::typeOrElseCall(const ResultCall &run, const Type *ret) {
  if (!this->returnsResult(run, ret)) {
    return;
  }
  const Type *common = ResultTypes::unifyHalf(run.value, ret->okType());
  if (common == nullptr) {
    std::string message = ErrorTextResultMethod::values(run.value, ret->okType());
    this->state.getDiagnostics().error(run.call->args[0]->loc(), message);
    return;
  }
  run.call->type = Type::result(common, ret->errType());
}
}
