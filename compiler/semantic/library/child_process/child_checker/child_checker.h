#pragma once

#include "compiler/semantic/library/child_process/child_checker/child_checker_errors.h"
#include "compiler/semantic/semantic_state/semantic_state.h"

namespace tz {

class ChildChecker {
public:
  explicit ChildChecker(SemanticState &owner);

  Expr *checkChildCall(CallExpr *call, llvm::StringRef fn, llvm::SMLoc loc);

private:
  static const Type *bufferType();
  static bool knownSignal(llvm::StringRef name);
  static bool hasZero(const Expr *expr);

  SemanticState &state;
};

}
