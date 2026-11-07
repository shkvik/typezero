#pragma once

#include "compiler/check/checker/checker.h"

namespace tz {

class OsChecker {
public:
  explicit OsChecker(Checker &owner);

  Expr *checkOsCall(CallExpr *call, llvm::StringRef fn, llvm::SMLoc loc);
  Expr *checkOsValue(llvm::StringRef name, Expr *expr);

private:
  static const Type *cpuInfoType();
  static const Type *userInfoType();

  Checker &checker;
};

}
