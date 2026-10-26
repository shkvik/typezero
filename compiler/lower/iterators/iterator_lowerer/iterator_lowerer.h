#pragma once

#include "compiler/lower/lowering/lowering.h"

namespace tz {

class IteratorLowerer {
public:
  explicit IteratorLowerer(Lowering &lowering);

  void emitIteratorForOf(ForOfStmt *stmt);
  void emitIteratorSpread(llvm::Value *array, Expr *source, const Type *element);
  llvm::Value *emitIteratorBuiltin(CallExpr *call);
  llvm::Value *emitIteratorSink(CallExpr *call);
  llvm::Value *emitNext(CallExpr *call);
  llvm::Value *emitIteratorObject(CallExpr *call);

private:
  Lowering &lowering_;
};

}
