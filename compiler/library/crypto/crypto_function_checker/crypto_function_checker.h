#pragma once

#include "compiler/check/checker/checker.h"

namespace tz {

class CryptoFunctionChecker {
public:
  explicit CryptoFunctionChecker(Checker &checker);

  Expr *checkCryptoCall(CallExpr *call, llvm::StringRef fn, llvm::SMLoc loc,
                        bool promisified = false);
  Expr *rejectLoosePromisify(CallExpr *call, llvm::SMLoc loc);

private:
  Expr *checkSyncCall(CallExpr *call, llvm::StringRef fn, llvm::StringRef shown, llvm::SMLoc loc);
  Expr *asyncOf(Expr *checked);

  Checker &checker_;
};

}
