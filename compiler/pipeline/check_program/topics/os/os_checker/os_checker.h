#pragma once

#include "compiler/pipeline/check_program/check_state/check_state.h"

namespace tz {

class OsChecker {
public:
  explicit OsChecker(CheckState &owner);

  Expr *checkOsCall(CallExpr *call, llvm::StringRef fn, llvm::SMLoc loc);
  Expr *checkOsValue(llvm::StringRef name, Expr *expr);

private:
  static const Type *cpuInfoType();
  static const Type *userInfoType();

  CheckState &state;
};

}
