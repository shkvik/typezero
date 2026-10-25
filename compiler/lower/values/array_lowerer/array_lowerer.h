#pragma once

#include "compiler/lower/lowering/lowering.h"

namespace tz {

// Arrays: literals, elements, length and for...of.
class ArrayLowerer {
public:
  explicit ArrayLowerer(Lowering &lowering);

  llvm::Value *emitElementAssign(AssignExpr *assign, bool needResult);
  llvm::Value *emitLengthAssign(AssignExpr *assign, bool needResult);
  llvm::Value *arrayField(llvm::Value *array, unsigned field);
  llvm::Value *arrayLength(llvm::Value *array); // as i64
  llvm::Value *arrayData(llvm::Value *array);
  llvm::Value *elementAddress(llvm::Value *data, const Type *element, llvm::Value *index);
  // Appends owned values, growing the array if needed.
  void emitPush(llvm::Value *array, llvm::ArrayRef<llvm::Value *> values, const Type *element);
  // A value read from a range-checked position: `T | undefined` unless the
  // position was proven in range.
  llvm::Value *emitCheckedRead(llvm::Value *inRange, const Type *resultType,
                               llvm::function_ref<llvm::Value *()> read);
  llvm::Value *emitArrayLiteral(ArrayLitExpr *lit);
  // `step(acc, element)` over the elements of a borrowed array of numbers or
  // booleans, starting from `init`; the last result.
  llvm::Value *emitArrayFold(llvm::Value *array, const Type *element, llvm::Value *init,
                             llvm::function_ref<llvm::Value *(llvm::Value *, llvm::Value *)> step);
  llvm::Value *emitNewArray(NewExpr *expr);
  llvm::Value *emitArrayIndex(IndexExpr *index);
  void emitArrayForOf(ForOfStmt *stmt, llvm::Value *iterableSlot, llvm::BasicBlock *condBlock,
                      llvm::BasicBlock *bodyBlock, llvm::BasicBlock *endBlock);

private:
  Lowering &lowering_;
};

} // namespace tz
