#pragma once

#include "compiler/check/checker/checker.h"

namespace tz {

class PathChecker {
public:
  explicit PathChecker(Checker &checker);

  Expr *checkPathCall(CallExpr *call, NodeModule module, llvm::StringRef fn, llvm::SMLoc loc);
  Expr *checkPathValue(NodeModule module, llvm::StringRef name, Expr *expr);

private:
  Checker &checker_;
};

}
