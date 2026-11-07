#pragma once

#include "compiler/pipeline/check_program/check_state/check_state.h"
#include "compiler/pipeline/check_program/topics/os/os_checker/os_checker_errors.h"
#include "compiler/result/result/result.h"

namespace tz {

class OsChecker {
public:
  explicit OsChecker(CheckState &owner);

  Expr *checkOsCall(CallExpr *call, llvm::StringRef fn, llvm::SMLoc loc);
  Expr *checkOsValue(llvm::StringRef name, Expr *expr);

private:
  using ResultOsFunction = Result<llvm::StringRef, OsCheckerOsFunctionError>;

  static ResultOsFunction osFunction(llvm::StringRef fn);
  static const Type *cpuInfoType();
  static const Type *userInfoType();

  CheckState &state;
};

}
