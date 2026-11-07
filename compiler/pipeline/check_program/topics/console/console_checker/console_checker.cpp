#include "compiler/pipeline/check_program/topics/console/console_checker/console_checker.h"
#include "compiler/pipeline/check_program/generics/arg_list_checker/arg_list_checker.h"
#include "compiler/pipeline/check_program/topics/expr/call_checker/call_checker.h"
#include "compiler/pipeline/check_program/topics/expr/expr_checker/expr_checker.h"
#include "llvm/ADT/STLExtras.h"
#include <cmath>

namespace tz {

ConsoleChecker::ConsoleChecker(CheckState &owner)
    : state(owner) {}

Expr *ConsoleChecker::checkConsoleLog(CallExpr *call) {
  call->target = BuiltinCall{.builtin = Builtin::ConsoleLog};
  call->type = Type::voidTy();
  for (size_t i = 0; i < call->args.size(); ++i) {
    Expr *&arg = call->args[i];
    arg = ExprChecker(this->state).checkExpr(arg);
    if (arg->type->isVoid()) {
      this->state.getDiagnostics().error(arg->loc(),
                                         "an expression of type 'void' cannot be printed");
    }
    const Type *type = arg->type;
    if (call->spreads(i) && !type->isArray() && !type->isListParam() &&
        !ArgListChecker::spreadsLists(type) && !type->isError()) {
      this->state.getDiagnostics().error(
          arg->loc(), "only an array can be spread into console.log, found '" + type->name() + "'");
    }
  }

  return call;
}

Expr *ConsoleChecker::checkConsoleCall(CallExpr *call, MemberExpr *member) {
  if (!CallChecker(this->state).isGlobalName(member->object, "console")) {
    return nullptr;
  }
  bool toStderr = member->name == "error" || member->name == "warn";
  if (toStderr || member->name == "log" || member->name == "info" || member->name == "debug") {
    this->checkConsoleLog(call);
    if (toStderr) {
      call->target = BuiltinCall{.builtin = Builtin::ConsoleError};
    }
    return call;
  }
  this->state.getDiagnostics().error(member->loc(),
                                     "'console." + member->name +
                                         "' is not supported yet; only 'console.log', 'info', "
                                         "'debug', 'error' and 'warn' are");
  for (Expr *&arg : call->args) {
    arg = ExprChecker(this->state).checkExpr(arg);
  }

  return call;
}

}
