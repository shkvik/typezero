#include "compiler/pipeline/check_program/topics/functions/function_method_checker/function_method_checker.h"
#include "compiler/pipeline/check_program/generics/arg_list_checker/arg_list_checker.h"
#include "compiler/pipeline/check_program/topics/expr/argument_checker/argument_checker.h"
#include "compiler/pipeline/check_program/topics/expr/call_checker/call_checker.h"
#include "compiler/pipeline/check_program/topics/expr/expr_checker/expr_checker.h"
#include "llvm/ADT/STLExtras.h"

namespace tz {

FunctionMethodChecker::FunctionMethodChecker(CheckState &owner)
    : state(owner) {}

FunctionMethodChecker::ResultThisCallMade FunctionMethodChecker::checkThisCall(CallExpr *call,
                                                                               MemberExpr *member) {
  const Type *type = member->object->type;
  call->type = Type::error();
  llvm::StringRef name = member->name;
  if (name == "bind") {
    ArgumentChecker(this->state).checkArgsLoosely(call);
    std::string message = ErrorTextThisCall::noBind();

    return err(FunctionMethodCheckerCheckThisCallError{
        .code = FunctionMethodCheckerCheckThisCallErrorCode::NO_BIND,
        .message = message,
    });
  }
  if (name != "call" && name != "apply") {
    ArgumentChecker(this->state).checkArgsLoosely(call);
    std::string message = ErrorTextThisCall::noProperty(name);

    return err(FunctionMethodCheckerCheckThisCallError{
        .code = FunctionMethodCheckerCheckThisCallErrorCode::NO_SUCH_PROPERTY,
        .message = message,
    });
  }
  if (call->args.empty()) {
    std::string message = ErrorTextThisCall::thisFirst(name);

    return err(FunctionMethodCheckerCheckThisCallError{
        .code = FunctionMethodCheckerCheckThisCallErrorCode::NO_THIS_ARGUMENT,
        .message = message,
    });
  }
  if (name == "apply" && (call->args.size() != 2 || call->hasSpread())) {
    ArgumentChecker(this->state).checkArgsLoosely(call);
    std::string message = ErrorTextThisCall::applyArgs();

    return err(FunctionMethodCheckerCheckThisCallError{
        .code = FunctionMethodCheckerCheckThisCallErrorCode::APPLY_ARGUMENTS,
        .message = message,
    });
  }

  if (name == "apply") {
    call->spread = {false, true};
    ArgListChecker(this->state).expandListSpreads(call->args, call->spread);
  }
  call->callee = member->object;
  call->type = type->ret();
  Expr *checked =
      ArgumentChecker(this->state)
          .checkArguments(call, type->params(), type->minArgs(), type->hasRest(), member->loc());

  return ok(checked);
}

FunctionMethodChecker::ResultParamsJoined
FunctionMethodChecker::checkListMethod(CallExpr *call, MemberExpr *member) {
  call->type = Type::error();
  if (member->name != "join") {
    ArgumentChecker(this->state).checkArgsLoosely(call);
    std::string message = ErrorTextListMethod::noMethod(member->name);

    return err(FunctionMethodCheckerCheckListMethodError{
        .code = FunctionMethodCheckerCheckListMethodErrorCode::NO_SUCH_METHOD,
        .message = message,
    });
  }
  if (CallChecker(this->state).rejectSpread(call)) {
    return err(FunctionMethodCheckerCheckListMethodError{
        .code = FunctionMethodCheckerCheckListMethodErrorCode::ALREADY_REPORTED,
        .message = "",
    });
  }
  ArgumentChecker::ResultArityMatches arity = ArgumentChecker(this->state).checkArity(call, 0, 1);
  if (arity.isErr()) {
    return err(FunctionMethodCheckerCheckListMethodError{
        .code = FunctionMethodCheckerCheckListMethodErrorCode::WRONG_ARITY,
        .message = arity.error().message,
    });
  }

  if (!call->args.empty()) {
    call->args[0] = ArgumentChecker(this->state).checkArg(call->args[0], Type::string());
  }
  call->type = Type::string();

  return ok(call);
}

}
