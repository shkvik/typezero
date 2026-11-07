#pragma once

#include "compiler/pipeline/check_program/check_state/check_state.h"
#include "compiler/pipeline/check_program/topics/path/path_checker/path_checker_errors.h"
#include "compiler/result/result/result.h"

namespace tz {

class PathChecker {
public:
  explicit PathChecker(CheckState &owner);

  Expr *checkPathCall(CallExpr *call, NodeModule module, llvm::StringRef fn, llvm::SMLoc loc);
  Expr *checkPathValue(NodeModule module, llvm::StringRef name, Expr *expr);

private:
  using ResultPathProperty = Result<llvm::StringRef, PathCheckerPathValueError>;
  using ResultPathFunction = Result<llvm::StringRef, PathCheckerPathFunctionError>;

  static ResultPathFunction pathFunction(llvm::StringRef fn);
  static ResultPathProperty pathValue(llvm::StringRef name, NodeModule module);
  static const Type *parsedPathType();
  static const Type *formatInputType();

  CheckState &state;
};

}
