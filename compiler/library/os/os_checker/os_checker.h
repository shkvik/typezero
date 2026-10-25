#pragma once

#include "compiler/check/checker/checker.h"

namespace tz {

// The functions of node:os.
class OsChecker {
public:
  explicit OsChecker(Checker &checker);

  Expr *checkOsCall(CallExpr *call, llvm::StringRef fn, llvm::SMLoc loc);

private:
  Checker &checker_;
};

} // namespace tz
