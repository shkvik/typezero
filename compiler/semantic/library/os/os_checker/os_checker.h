#pragma once

#include "compiler/result/result/result.h"
#include "compiler/semantic/library/os/os_checker/os_checker_errors.h"
#include "compiler/semantic/semantic_state/semantic_state.h"

namespace tz {

class OsChecker {
public:
  explicit OsChecker(SemanticState &owner);

  Expr *checkOsCall(CallExpr *call, llvm::StringRef fn, llvm::SMLoc loc);
  Expr *checkOsValue(llvm::StringRef name, Expr *expr);

private:
  using ResultOsFunction = Result<llvm::StringRef, OsCheckerOsFunctionError>;

  static ResultOsFunction osFunction(llvm::StringRef fn);
  static const Type *cpuInfoType();
  static const Type *userInfoType();

  SemanticState &state;
};

}
