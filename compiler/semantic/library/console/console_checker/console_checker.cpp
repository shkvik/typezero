#include "compiler/semantic/library/console/console_checker/console_checker.h"
#include "compiler/program/library/builtin_names/global_name_table/global_name_table.h"
#include "compiler/semantic/bodies/expr/call_checker/call_checker.h"
#include "compiler/semantic/bodies/expr/expr_checker/expr_checker.h"
#include "compiler/semantic/generics/arg_list_checker/arg_list_checker.h"
#include "compiler/semantic/semantic_parts/semantic_parts.h"
#include "llvm/ADT/STLExtras.h"
#include <cmath>

namespace tz {

ConsoleChecker::ConsoleChecker(SemanticState &owner, SemanticParts &all)
    : state(owner) {
  this->callChecker = &all.getCallChecker();
  this->exprChecker = &all.getExprChecker();
}

ConsoleChecker::ResultPrintableItem ConsoleChecker::printable(const Expr *arg) {
  if (arg->type->isVoid()) {
    std::string message = ErrorTextConsole::voidValue();

    return err(ConsoleCheckerPrintableError{
        .code = ConsoleCheckerPrintableErrorCode::VOID_VALUE,
        .message = message,
    });
  }

  return ok(arg);
}

ConsoleChecker::ResultSpreadableArg ConsoleChecker::spreadable(const CallExpr *call, size_t i) {
  const Type *type = call->args[i]->type;
  bool spreadsArray = !call->spreads(i) || type->isArray() || type->isListParam() ||
                      ArgListChecker::spreadsLists(type) || type->isError();
  if (!spreadsArray) {
    std::string message = ErrorTextConsole::spread(type->name());

    return err(ConsoleCheckerSpreadableError{
        .code = ConsoleCheckerSpreadableErrorCode::NOT_AN_ARRAY,
        .message = message,
    });
  }

  return ok(call->args[i]);
}

ConsoleChecker::ResultConsoleMethod ConsoleChecker::consoleMethod(const MemberExpr *member) {
  llvm::StringRef name = member->name;
  bool printing =
      name == "error" || name == "warn" || name == "log" || name == "info" || name == "debug";
  if (!printing) {
    std::string message = ErrorTextConsole::notYet(member->name);

    return err(ConsoleCheckerConsoleMethodError{
        .code = ConsoleCheckerConsoleMethodErrorCode::NOT_SUPPORTED_YET,
        .message = message,
    });
  }

  return ok(member);
}

Expr *ConsoleChecker::checkConsoleLog(CallExpr *call) {
  call->target = BuiltinCall{.builtin = Builtin::ConsoleLog};
  call->type = Type::voidTy();
  for (size_t i = 0; i < call->args.size(); ++i) {
    Expr *&arg = call->args[i];
    arg = this->exprChecker->checkExpr(arg);
    ResultPrintableItem printable = ConsoleChecker::printable(arg);
    if (printable.isErr()) {
      this->state.getDiagnostics().error(arg->loc(), printable.error().message);
    }
    ResultSpreadableArg spreadable = ConsoleChecker::spreadable(call, i);
    if (spreadable.isErr()) {
      this->state.getDiagnostics().error(arg->loc(), spreadable.error().message);
    }
  }

  return call;
}

Expr *ConsoleChecker::checkConsoleCall(CallExpr *call, MemberExpr *member) {
  if (!this->callChecker->isGlobalName(member->object, GlobalName::CONSOLE)) {
    return nullptr;
  }
  ResultConsoleMethod method = ConsoleChecker::consoleMethod(member);
  if (method.isErr()) {
    this->state.getDiagnostics().error(member->loc(), method.error().message);
    for (Expr *&arg : call->args) {
      arg = this->exprChecker->checkExpr(arg);
    }
    return call;
  }
  this->checkConsoleLog(call);
  bool toStderr = member->name == "error" || member->name == "warn";
  if (toStderr) {
    call->target = BuiltinCall{.builtin = Builtin::ConsoleError};
  }

  return call;
}

}
