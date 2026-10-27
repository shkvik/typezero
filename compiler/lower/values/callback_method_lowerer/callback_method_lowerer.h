#pragma once

#include "compiler/lower/lowering/lowering.h"

namespace tz {

class CallbackMethodLowerer {
public:
  explicit CallbackMethodLowerer(Lowering &lowering);

  llvm::Value *emitCallbackMethod(CallExpr *call);
  llvm::Function *sortComparator(CallExpr *call);
  llvm::Value *firstOfReduce(CallExpr *call, llvm::Value *length, llvm::Value *start,
                             llvm::Value *step, llvm::Value *accSlot, const Type *accType,
                             llvm::function_ref<llvm::Value *(llvm::Value *)> readElement);
  llvm::Value *reduceResult(CallExpr *call, llvm::Value *length, llvm::Value *accSlot,
                            const Type *accType);

private:
  Lowering &lowering_;
};

}
