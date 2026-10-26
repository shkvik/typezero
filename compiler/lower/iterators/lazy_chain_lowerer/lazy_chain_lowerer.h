#pragma once

#include "compiler/lower/lowering/lowering.h"

namespace tz {

class LazyChainLowerer {
public:
  explicit LazyChainLowerer(Lowering &lowering);

  Lowering::Producer valueProducer(llvm::Value *slot, const Type *type,
                                   llvm::Value *returnSlot = nullptr);
  Lowering::Producer arrayProducer(llvm::Value *slot, const Type *arrayType, Builtin mode);
  Lowering::Consumer stageConsumer(Builtin stage, const Type *callbackType, const Type *element,
                                   const Type *result, llvm::Value *operand,
                                   const Lowering::Consumer &down);
  llvm::Value *callCallback(llvm::Value *slot, const Type *fnType, std::vector<llvm::Value *> args,
                            llvm::ArrayRef<const Type *> types);

private:
  Lowering &lowering_;
};

}
