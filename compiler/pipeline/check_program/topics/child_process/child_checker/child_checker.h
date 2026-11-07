#pragma once

#include "compiler/pipeline/check_program/check_state/check_state.h"
#include "compiler/pipeline/check_program/topics/child_process/child_checker/child_checker_errors.h"

namespace tz {

class ChildChecker {
public:
  explicit ChildChecker(CheckState &owner);

  Expr *checkChildCall(CallExpr *call, llvm::StringRef fn, llvm::SMLoc loc);

private:
  static const Type *bufferType();
  static bool knownSignal(llvm::StringRef name);
  static bool hasZero(const Expr *expr);

  CheckState &state;
};

}
