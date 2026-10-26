#pragma once

#include "compiler/lower/lowering/lowering.h"

namespace tz {

class AssignLowerer {
public:
  explicit AssignLowerer(Lowering &lowering);

  llvm::Value *emitAssign(AssignExpr *assign, bool needResult);
  llvm::Value *emitNullishAssign(AssignExpr *assign, bool needResult);
  llvm::Value *emitUpdate(UpdateExpr *update);
  static bool cannotAssign(const Expr *expr);

private:
  Lowering &lowering_;
};

}
