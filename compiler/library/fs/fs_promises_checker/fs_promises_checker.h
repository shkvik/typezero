#pragma once

#include "compiler/check/checker/checker.h"

namespace tz {

class FsPromisesChecker {
public:
  explicit FsPromisesChecker(Checker &checker);

  Expr *checkFsPromisesCall(CallExpr *call, llvm::StringRef fn, llvm::SMLoc loc);

private:
  Checker &checker_;
};

}
