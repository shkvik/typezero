#pragma once

#include "compiler/check/checker/checker.h"

namespace tz {

// node:readline.
class ReadlineChecker {
public:
  explicit ReadlineChecker(Checker &checker);

  Expr *checkReadlineCall(CallExpr *call, NodeModule module, llvm::StringRef fn, llvm::SMLoc loc);

private:
  Checker &checker_;
};

} // namespace tz
