#pragma once

#include "compiler/check/checker/checker.h"

namespace tz {

// The functions of node:path.
class PathChecker {
public:
  explicit PathChecker(Checker &checker);

  // The calls of the functions of each module, by name.
  Expr *checkPathCall(CallExpr *call, NodeModule module, llvm::StringRef fn, llvm::SMLoc loc);

private:
  Checker &checker_;
};

} // namespace tz
