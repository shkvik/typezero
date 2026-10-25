#pragma once

#include "compiler/lower/lowering/lowering.h"

namespace tz {

// Lazy chains of iterator helpers, run where they are consumed.
class LazyChainLowerer {
public:
  explicit LazyChainLowerer(Lowering &lowering);

  // The iteration of a value held in `slot`: an array, a string or an
  // iterator object, whose generator's return value goes to `returnSlot`.
  Lowering::Producer valueProducer(llvm::Value *slot, const Type *type,
                                   llvm::Value *returnSlot = nullptr);
  Lowering::Producer arrayProducer(llvm::Value *slot, const Type *arrayType, Builtin mode);
  // The stage `stage` of a chain (map, filter...) before `down`: it gets
  // values of `element` and gives values of `result`. `operand` is the slot
  // of its callback, of `callbackType`, or of its count.
  Lowering::Consumer stageConsumer(Builtin stage, const Type *callbackType, const Type *element,
                                   const Type *result, llvm::Value *operand,
                                   const Lowering::Consumer &down);
  // Calls a callback held in `slot` with owned arguments of `types`, as many
  // as it takes; the others are released.
  llvm::Value *callCallback(llvm::Value *slot, const Type *fnType, std::vector<llvm::Value *> args,
                            llvm::ArrayRef<const Type *> types);

private:
  Lowering &lowering_;
};

} // namespace tz
