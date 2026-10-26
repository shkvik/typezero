#pragma once

#include "compiler/check/checker/checker.h"

namespace tz {

class ReturnChecker {
public:
  explicit ReturnChecker(Checker &checker);

  void checkReturn(ReturnStmt *ret);
  Expr *checkReturnValue(ReturnStmt *ret, const Type *hint, bool tested);
  static bool alwaysReturns(const Stmt *stmt, llvm::ArrayRef<llvm::StringRef> labels = {});
  static bool isExit(const Stmt *stmt);

private:
  Checker &checker_;
};

}
