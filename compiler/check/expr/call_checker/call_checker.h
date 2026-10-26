#pragma once

#include "compiler/check/checker/checker.h"

namespace tz {

class CallChecker {
public:
  explicit CallChecker(Checker &checker);

  Expr *checkValueCall(CallExpr *call, const Type *fnType);
  Expr *checkCall(CallExpr *call, const Type *expected);
  Expr *checkDirectCall(CallExpr *call, FuncSymbol *func, llvm::SMLoc loc,
                        const Type *expected = nullptr);
  bool rejectSpread(CallExpr *call);
  bool isGlobalName(const Expr *expr, llvm::StringRef name) const;

private:
  Checker &checker_;
};

}
