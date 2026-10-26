#pragma once

#include "compiler/check/checker/checker.h"

namespace tz {

class ReadlineChecker {
public:
  explicit ReadlineChecker(Checker &checker);

  Expr *checkReadlineCall(CallExpr *call, NodeModule module, llvm::StringRef fn, llvm::SMLoc loc);

private:
  Checker &checker_;
};

}
