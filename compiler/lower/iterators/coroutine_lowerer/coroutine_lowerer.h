#pragma once

#include "compiler/lower/lowering/lowering.h"

namespace tz {

class CoroutineLowerer {
public:
  explicit CoroutineLowerer(Lowering &lowering);

  Lowering::EmitContext saveContext();
  void restoreContext(const Lowering::EmitContext &context);
  llvm::Value *emitResume(llvm::Value *it, llvm::Value *out, llvm::Value *in, llvm::Value *ret);
  void beginCoroutine(unsigned layoutFlags, const Type *sent, llvm::StructType *promise = nullptr);
  void emitSuspend();
  llvm::Value *emitCoroutineYield(llvm::Value *value, const Type *resultType);
  void emitCoroutineReturn(llvm::Value *value, const Type *type);
  void finishCoroutine();
  llvm::Function *coroIntrinsic(llvm::Intrinsic::ID id, llvm::ArrayRef<llvm::Type *> types = {});

private:
  Lowering &lowering_;
};

}
