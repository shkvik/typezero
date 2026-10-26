#pragma once

#include "compiler/check/checker/checker.h"

namespace tz {

class KeyPairChecker {
public:
  explicit KeyPairChecker(Checker &checker);

  Expr *checkGenerateKeyPair(CallExpr *call, llvm::SMLoc loc);

private:
  Checker &checker_;
};

}
