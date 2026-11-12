#include "compiler/semantic/bodies/functions/function_method_checker/function_method_checker.h"
#include "compiler/program/library/array/array_method_table/array_method_table.h"
#include "compiler/program/library/function/function_method_table/function_method_table.h"
#include "compiler/semantic/bodies/expr/argument_checker/argument_checker.h"
#include "compiler/semantic/bodies/expr/call_checker/call_checker.h"
#include "compiler/semantic/bodies/expr/expr_checker/expr_checker.h"
#include "compiler/semantic/generics/arg_list_checker/arg_list_checker.h"
#include "compiler/semantic/semantic_parts/semantic_parts.h"
#include "llvm/ADT/STLExtras.h"

namespace tz {

FunctionMethodChecker::FunctionMethodChecker(SemanticState &owner, SemanticParts &all)
    : state(owner) {
  this->callChecker = &all.callChecker;
  this->argListChecker = &all.argListChecker;
  this->argumentChecker = &all.argumentChecker;
}

FunctionMethodChecker::ResultThisCallMade FunctionMethodChecker::checkThisCall(CallExpr *call,
                                                                               MemberExpr *member) {
  call->type = Type::error();
  auto method = this->checkThisMethod(call, member);
  if (method.isErr()) {
    return err(method.error());
  }
  auto arguments = this->checkThisArguments(call, member, method.value());
  if (arguments.isErr()) {
    return err(arguments.error());
  }
  auto *checked = this->checkThisArgumentTypes(call, member, method.value());

  return ok(checked);
}

FunctionMethodChecker::ResultThisMethodKnown
FunctionMethodChecker::checkThisMethod(CallExpr *call, const MemberExpr *member) {
  auto method = FunctionMethodTable::find(member->name);
  if (method == FunctionMethod::BIND) {
    this->argumentChecker->walkArgsLoosely(call);
    auto message = ErrorTextThisCall::noBind();

    return err(FunctionMethodCheckerCheckThisCallError{
        .code = FunctionMethodCheckerCheckThisCallErrorCode::NO_BIND,
        .message = message,
    });
  }
  if (!method.has_value()) {
    this->argumentChecker->walkArgsLoosely(call);
    auto message = ErrorTextThisCall::noProperty(member->name);

    return err(FunctionMethodCheckerCheckThisCallError{
        .code = FunctionMethodCheckerCheckThisCallErrorCode::NO_SUCH_PROPERTY,
        .message = message,
    });
  }

  return ok(*method);
}

FunctionMethodChecker::ResultThisArgumentsFit
FunctionMethodChecker::checkThisArguments(CallExpr *call, const MemberExpr *member,
                                          FunctionMethod method) {
  if (call->args.empty()) {
    auto message = ErrorTextThisCall::thisFirst(member->name);

    return err(FunctionMethodCheckerCheckThisCallError{
        .code = FunctionMethodCheckerCheckThisCallErrorCode::NO_THIS_ARGUMENT,
        .message = message,
    });
  }
  bool isApplyShape = call->args.size() == 2 && !call->hasSpread();
  if (method == FunctionMethod::APPLY && !isApplyShape) {
    this->argumentChecker->walkArgsLoosely(call);
    auto message = ErrorTextThisCall::applyArgs();

    return err(FunctionMethodCheckerCheckThisCallError{
        .code = FunctionMethodCheckerCheckThisCallErrorCode::APPLY_ARGUMENTS,
        .message = message,
    });
  }

  return ok(call);
}

Expr *FunctionMethodChecker::checkThisArgumentTypes(CallExpr *call, MemberExpr *member,
                                                    FunctionMethod method) {
  const Type *type = member->object->type;
  if (method == FunctionMethod::APPLY) {
    call->spread = {false, true};
    this->argListChecker->expandListSpreads(call->args, call->spread);
  }
  call->callee = member->object;
  call->type = type->ret();

  return this->argumentChecker->checkArguments(call, type->params(), type->minArgs(),
                                               type->hasRest(), member->loc());
}

FunctionMethodChecker::ResultParamsJoined
FunctionMethodChecker::checkListMethod(CallExpr *call, MemberExpr *member) {
  call->type = Type::error();
  const auto *method = ArrayMethodTable::find(member->name);
  bool isJoining = method != nullptr && method->method == ArrayMethod::JOIN;
  if (!isJoining) {
    this->argumentChecker->walkArgsLoosely(call);
    auto message = ErrorTextListMethod::noMethod(member->name);

    return err(FunctionMethodCheckerCheckListMethodError{
        .code = FunctionMethodCheckerCheckListMethodErrorCode::NO_SUCH_METHOD,
        .message = message,
    });
  }
  if (this->callChecker->rejectSpread(call)) {
    return err(FunctionMethodCheckerCheckListMethodError{
        .code = FunctionMethodCheckerCheckListMethodErrorCode::ALREADY_REPORTED,
        .message = "",
    });
  }
  auto arity = this->argumentChecker->checkArity(call, 0, 1);
  if (arity.isErr()) {
    return err(FunctionMethodCheckerCheckListMethodError{
        .code = FunctionMethodCheckerCheckListMethodErrorCode::WRONG_ARITY,
        .message = arity.error().message,
    });
  }

  if (!call->args.empty()) {
    call->args[0] = this->argumentChecker->checkArg(call->args[0], Type::string());
  }
  call->type = Type::string();

  return ok(call);
}

}
