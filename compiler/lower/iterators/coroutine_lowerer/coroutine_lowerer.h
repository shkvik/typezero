#pragma once

#include "compiler/lower/lowering/lowering.h"

namespace tz {

// Coroutines: their frames, suspension and resumption.
class CoroutineLowerer {
public:
  explicit CoroutineLowerer(Lowering &lowering);

  Lowering::EmitContext saveContext();
  void restoreContext(const Lowering::EmitContext &context);
  // Resumes the iterator `it`: true if it gave a value, which is in `out`.
  llvm::Value *emitResume(llvm::Value *it, llvm::Value *out, llvm::Value *in, llvm::Value *ret);
  // `promise`, if given, is the type of a promise that holds more than
  // promiseTy_, which it starts with.
  void beginCoroutine(unsigned layoutFlags, const Type *sent, llvm::StructType *promise = nullptr);
  // A suspension that is not the last; the builder goes on where the
  // coroutine resumes.
  void emitSuspend();
  llvm::Value *emitCoroutineYield(llvm::Value *value, const Type *resultType);
  void emitCoroutineReturn(llvm::Value *value, const Type *type);
  void finishCoroutine();
  llvm::Function *coroIntrinsic(llvm::Intrinsic::ID id, llvm::ArrayRef<llvm::Type *> types = {});

private:
  Lowering &lowering_;
};

} // namespace tz
