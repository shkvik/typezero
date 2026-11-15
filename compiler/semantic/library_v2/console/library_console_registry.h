#pragma once

#include "compiler/program/library/console/console_method_table/console_method_table.h"
#include "compiler/result/result/result.h"
#include "compiler/semantic/library_v2/console/library_console_registry_errors.h"
#include "compiler/semantic/semantic_state/v2/semantic_state.h"

namespace tz {

class LibraryConsoleRegistry {
public:
  explicit LibraryConsoleRegistry(SemanticState &owner);

  void checkConsoleCall(const CheckConsoleCall &task);
  void nextConsoleArgument(const NextConsoleArgument &task);
  void afterConsoleArgument(const AfterConsoleArgument &task);

private:
  using ResultPrintableItem = Result<const Expr *, LibraryConsoleRegistryPrintableError>;
  using ResultSpreadableArg = Result<const Expr *, LibraryConsoleRegistrySpreadableError>;
  using ResultConsoleMethod = Result<ConsoleMethod, LibraryConsoleRegistryConsoleMethodError>;

  ResultPrintableItem checkPrintable(const Expr *arg) const;
  ResultConsoleMethod checkConsoleMethod(const MemberExpr *member) const;
  ResultSpreadableArg checkSpreadable(const CallExpr *call, size_t index) const;

  SemanticState &state;
};

}
