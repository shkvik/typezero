#pragma once

#include "compiler/check/checker/checker.h"

namespace tz {

// return, and whether a function always returns.
class ReturnChecker {
public:
  explicit ReturnChecker(Checker &checker);

  void checkReturn(ReturnStmt *ret);
  Expr *checkReturnValue(ReturnStmt *ret, const Type *hint, bool tested);
  // Whether control can never reach the end of `stmt`; `labels` are its own.
  static bool alwaysReturns(const Stmt *stmt, llvm::ArrayRef<llvm::StringRef> labels = {});
  // A checked `process.exit()` statement, after which nothing runs.
  static bool isExit(const Stmt *stmt);

private:
  Checker &checker_;
};

} // namespace tz
