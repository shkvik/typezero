#pragma once

#include "compiler/lower/lowering/lowering.h"

namespace tz {

// Assignments, ++ and --.
class AssignLowerer {
public:
  explicit AssignLowerer(Lowering &lowering);

  llvm::Value *emitAssign(AssignExpr *assign, bool needResult);
  llvm::Value *emitNullishAssign(AssignExpr *assign, bool needResult);
  llvm::Value *emitUpdate(UpdateExpr *update);
  // Whether evaluating `expr` cannot assign to any variable.
  static bool cannotAssign(const Expr *expr);

private:
  Lowering &lowering_;
};

} // namespace tz
