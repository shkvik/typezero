#pragma once

#include "compiler/lower/codegen/codegen.h"

namespace tz {

// Assignments, ++ and --.
class AssignLowerer {
public:
  explicit AssignLowerer(CodeGen &codegen);

  llvm::Value *emitAssign(AssignExpr *assign, bool needResult);
  llvm::Value *emitNullishAssign(AssignExpr *assign, bool needResult);
  llvm::Value *emitUpdate(UpdateExpr *update);
  // Whether evaluating `expr` cannot assign to any variable.
  static bool cannotAssign(const Expr *expr);

private:
  CodeGen &codegen_;
};

} // namespace tz
