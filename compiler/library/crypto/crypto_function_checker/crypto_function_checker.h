#pragma once

#include "compiler/check/checker/checker.h"

namespace tz {

class CryptoFunctionChecker {
public:
  explicit CryptoFunctionChecker(Checker &checker);

  Expr *checkCryptoCall(CallExpr *call, llvm::StringRef fn, llvm::SMLoc loc);

private:
  Checker &checker_;
};

}
