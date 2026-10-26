#pragma once

#include "compiler/check/checker/checker.h"

namespace tz {

class ChildChecker {
public:
  explicit ChildChecker(Checker &checker);

  Expr *checkChildCall(CallExpr *call, llvm::StringRef fn, llvm::SMLoc loc);
  void declareExecSyncError();

private:
  Checker &checker_;
};

}
