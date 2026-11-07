#include "compiler/library/console/console_checker/console_checker.h"
#include "compiler/check/expr/call_checker/call_checker.h"
#include "compiler/check/expr/expr_checker/expr_checker.h"
#include "llvm/ADT/STLExtras.h"
#include <cmath>


namespace tz {

ConsoleChecker::ConsoleChecker(Checker &owner)
    : checker(owner) {}

Expr *ConsoleChecker::checkConsoleLog(CallExpr *call) {
  call->target = BuiltinCall{.builtin = Builtin::ConsoleLog};
  call->type = Type::voidTy();
  for (size_t i = 0; i < call->args.size(); ++i) {
    Expr *&arg = call->args[i];
    arg = this->checker.exprChecker().checkExpr(arg);
    if (arg->type->isVoid()) {
      this->checker.diags_.error(arg->loc(), "an expression of type 'void' cannot be printed");
    }
    const Type *type = arg->type;
    if (call->spreads(i) && !type->isArray() && !type->isListParam() && !spreadsLists(type) &&
        !type->isError()) {
      this->checker.diags_.error(
          arg->loc(), "only an array can be spread into console.log, found '" + type->name() + "'");
    }
  }

  return call;
}

Expr *ConsoleChecker::checkConsoleCall(CallExpr *call, MemberExpr *member) {
  if (!this->checker.callChecker().isGlobalName(member->object, "console")) {
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
  this->checker.diags_.error(member->loc(),
                             "'console." + member->name +
                                 "' is not supported yet; only 'console.log', 'info', "
                                 "'debug', 'error' and 'warn' are");
  for (Expr *&arg : call->args) {
    arg = this->checker.exprChecker().checkExpr(arg);
  }

  return call;
}

}
