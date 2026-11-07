#pragma once

#include "compiler/lower/lowering/lowering.h"

namespace tz {

class TruthLowerer {
public:
  explicit TruthLowerer(Lowering &owner);

  llvm::Value *emitLogical(BinaryExpr *bin);
  llvm::Value *decidedValue(BinaryExpr *bin, llvm::Value *lhs);
  llvm::Value *emitTruth(llvm::Value *value, const Type *type);
  llvm::Value *emitTruthOf(Expr *expr);

private:
  static bool isReference(const Type *type);

  Lowering &lowering;
};

}
