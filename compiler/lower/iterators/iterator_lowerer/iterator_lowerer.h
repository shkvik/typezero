#pragma once

#include "compiler/lower/lowering/lowering.h"

namespace tz {

// Iterators: for...of, spreads, next and the built-ins of iterators.
class IteratorLowerer {
public:
  explicit IteratorLowerer(Lowering &lowering);

  void emitIteratorForOf(ForOfStmt *stmt);
  void emitIteratorSpread(llvm::Value *array, Expr *source, const Type *element);
  llvm::Value *emitIteratorBuiltin(CallExpr *call);
  llvm::Value *emitIteratorSink(CallExpr *call);
  llvm::Value *emitNext(CallExpr *call);
  // An iterator object for a helper or the iterator of an array.
  llvm::Value *emitIteratorObject(CallExpr *call);

private:
  Lowering &lowering_;
};

} // namespace tz
