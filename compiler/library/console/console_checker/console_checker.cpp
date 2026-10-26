#include "compiler/library/console/console_checker/console_checker.h"
#include "compiler/check/expr/call_checker/call_checker.h"
#include "compiler/check/expr/expr_checker/expr_checker.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/ADT/StringSet.h"
#include "llvm/Support/ErrorHandling.h"
#include <cmath>
#include <functional>

using llvm::cast;
using llvm::dyn_cast;
using llvm::dyn_cast_or_null;
using llvm::isa;

namespace tz {

ConsoleChecker::ConsoleChecker(Checker &checker) : checker_(checker) {}

Expr *ConsoleChecker::checkConsoleLog(CallExpr *call) {
  call->builtin = Builtin::ConsoleLog;
  call->type = Type::voidTy();
  for (size_t i = 0; i < call->args.size(); ++i) {
    Expr *&arg = call->args[i];
    arg = checker_.exprChecker().checkExpr(arg);
    if (arg->type->isVoid()) {
      checker_.diags_.error(arg->loc(), "an expression of type 'void' cannot be printed");
    }
    const Type *type = arg->type;
    if (call->spreads(i) && !type->isArray() && !type->isListParam() && !spreadsLists(type) &&
        !type->isError()) {
      checker_.diags_.error(arg->loc(), "only an array can be spread into console.log, found '" +
                                            type->name() + "'");
    }
  }

  return call;
}

Expr *ConsoleChecker::checkConsoleCall(CallExpr *call, MemberExpr *member) {
  if (!checker_.callChecker().isGlobalName(member->object, "console")) {
    return nullptr;
  }
  bool toStderr = member->name == "error" || member->name == "warn";
  if (toStderr || member->name == "log" || member->name == "info" || member->name == "debug") {
    checkConsoleLog(call);
    if (toStderr) {
      call->builtin = Builtin::ConsoleError;
    }
    return call;
  }
  checker_.diags_.error(member->loc(), "'console." + member->name +
                                           "' is not supported yet; only 'console.log', 'info', "
                                           "'debug', 'error' and 'warn' are");
  for (Expr *&arg : call->args) {
    arg = checker_.exprChecker().checkExpr(arg);
  }

  return call;
}

}
