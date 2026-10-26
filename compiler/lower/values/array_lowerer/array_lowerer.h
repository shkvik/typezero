#pragma once

#include "compiler/lower/lowering/lowering.h"

namespace tz {

class ArrayLowerer {
public:
  explicit ArrayLowerer(Lowering &lowering);

  llvm::Value *emitElementAssign(AssignExpr *assign, bool needResult);
  llvm::Value *emitLengthAssign(AssignExpr *assign, bool needResult);
  llvm::Value *arrayField(llvm::Value *array, unsigned field);
  llvm::Value *arrayLength(llvm::Value *array);
  llvm::Value *arrayData(llvm::Value *array);
  llvm::Value *elementAddress(llvm::Value *data, const Type *element, llvm::Value *index);
  void emitPush(llvm::Value *array, llvm::ArrayRef<llvm::Value *> values, const Type *element);
  llvm::Value *emitCheckedRead(llvm::Value *inRange, const Type *resultType,
                               llvm::function_ref<llvm::Value *()> read);
  llvm::Value *emitArrayLiteral(ArrayLitExpr *lit);
  llvm::Value *emitArrayFold(llvm::Value *array, const Type *element, llvm::Value *init,
                             llvm::function_ref<llvm::Value *(llvm::Value *, llvm::Value *)> step);
  llvm::Value *emitNewArray(NewExpr *expr);
  llvm::Value *emitArrayIndex(IndexExpr *index);
  void emitArrayForOf(ForOfStmt *stmt, llvm::Value *iterableSlot, llvm::BasicBlock *condBlock,
                      llvm::BasicBlock *bodyBlock, llvm::BasicBlock *endBlock);

private:
  Lowering &lowering_;
};

}
