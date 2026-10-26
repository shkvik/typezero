#pragma once

#include "compiler/check/checker/checker.h"

namespace tz {

class GlobalFunctionChecker {
public:
  explicit GlobalFunctionChecker(Checker &checker);

  Expr *checkGlobalCall(CallExpr *call, NameExpr *name, const Type *expected);
  Expr *checkGlobalFunction(CallExpr *call, llvm::StringRef fn, llvm::SMLoc loc,
                            const Type *expected);
  Expr *reportLegacyEscape(CallExpr *call, llvm::StringRef fn, llvm::SMLoc loc);

private:
  Checker &checker_;
};

}
