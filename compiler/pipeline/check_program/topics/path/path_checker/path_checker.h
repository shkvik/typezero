#pragma once

#include "compiler/pipeline/check_program/check_state/check_state.h"

namespace tz {

class PathChecker {
public:
  explicit PathChecker(CheckState &owner);

  Expr *checkPathCall(CallExpr *call, NodeModule module, llvm::StringRef fn, llvm::SMLoc loc);
  Expr *checkPathValue(NodeModule module, llvm::StringRef name, Expr *expr);

private:
  static const Type *parsedPathType();
  static const Type *formatInputType();

  CheckState &state;
};

}
