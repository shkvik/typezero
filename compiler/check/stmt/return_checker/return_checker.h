#pragma once

#include "compiler/check/checker/checker.h"

namespace tz {

class ReturnChecker {
public:
  explicit ReturnChecker(Checker &owner);

  void checkReturn(ReturnStmt *ret);
  Expr *checkReturnValue(ReturnStmt *ret, const Type *hint, bool tested);
  static bool alwaysReturns(const Stmt *stmt, llvm::ArrayRef<llvm::StringRef> labels = {});
  static bool isExit(const Stmt *stmt);

private:
  static bool breaksTo(const Stmt *stmt, llvm::ArrayRef<llvm::StringRef> labels);

  Checker &checker;
};

}
