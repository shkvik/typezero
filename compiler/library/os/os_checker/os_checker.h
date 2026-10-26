#pragma once

#include "compiler/check/checker/checker.h"

namespace tz {

class OsChecker {
public:
  explicit OsChecker(Checker &checker);

  Expr *checkOsCall(CallExpr *call, llvm::StringRef fn, llvm::SMLoc loc);
  Expr *checkOsValue(llvm::StringRef name, Expr *expr);

private:
  Checker &checker_;
};

}
