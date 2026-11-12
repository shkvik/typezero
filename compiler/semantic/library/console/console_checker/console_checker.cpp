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
  this->callChecker = &all.callChecker;
  this->exprChecker = &all.exprChecker;
}

ConsoleChecker::ResultPrintableItem ConsoleChecker::checkPrintable(const Expr *arg) const {
  if (arg->type->isVoid()) {
    auto message = ErrorTextConsole::voidValue();

    return err(ConsoleCheckerPrintableError{
        .code = ConsoleCheckerPrintableErrorCode::VOID_VALUE,
        .message = message,
    });
  }

  return ok(arg);
}

ConsoleChecker::ResultSpreadableArg ConsoleChecker::checkSpreadable(const CallExpr *call,
                                                                    size_t index) const {
  const Type *type = call->args[index]->type;
  bool isSpreadingArray = !call->spreads(index) || type->isArray() || type->isListParam() ||
                          ArgListChecker::isSpreadingLists(type) || type->isError();
  if (!isSpreadingArray) {
    auto message = ErrorTextConsole::spread(type->name());

    return err(ConsoleCheckerSpreadableError{
        .code = ConsoleCheckerSpreadableErrorCode::NOT_AN_ARRAY,
        .message = message,
    });
  }

  return ok(call->args[index]);
}

ConsoleChecker::ResultConsoleMethod
ConsoleChecker::checkConsoleMethod(const MemberExpr *member) const {
  auto method = ConsoleMethodTable::find(member->name);
  if (!method.has_value()) {
    auto message = ErrorTextConsole::notYet(member->name);

    return err(ConsoleCheckerConsoleMethodError{
        .code = ConsoleCheckerConsoleMethodErrorCode::NOT_SUPPORTED_YET,
        .message = message,
    });
  }

  return ok(method.value());
}

Expr *ConsoleChecker::checkConsoleLog(CallExpr *call) {
  call->target = BuiltinCall{.builtin = Builtin::ConsoleLog};
  call->type = Type::voidTy();
  for (size_t index = 0; index < call->args.size(); ++index) {
    Expr *&arg = call->args[index];
    arg = this->exprChecker->checkExpr(arg);
    auto printable = this->checkPrintable(arg);
    if (printable.isErr()) {
      this->state.getDiagnostics().error(arg->loc(), printable.error().message);
    }
    auto spreadable = this->checkSpreadable(call, index);
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
  auto method = this->checkConsoleMethod(member);
  if (method.isErr()) {
    this->state.getDiagnostics().error(member->loc(), method.error().message);
    for (Expr *&arg : call->args) {
      arg = this->exprChecker->checkExpr(arg);
    }
    return call;
  }
  this->checkConsoleLog(call);
  if (ConsoleMethodTable::isToStderr(method.value())) {
    call->target = BuiltinCall{.builtin = Builtin::ConsoleError};
  }

  return call;
}

}
