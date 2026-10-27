#pragma once

#include "compiler/lower/lowering/lowering.h"

namespace tz {

class PromiseLowerer {
public:
  explicit PromiseLowerer(Lowering &lowering);

  uint64_t promiseValueSize(const Type *value);
  llvm::Value *promiseValueAddress(llvm::Value *promise);
  llvm::Value *promiseField(llvm::Value *promise, unsigned field);
  llvm::Constant *promiseLayout(const Type *value);
  llvm::Value *newPromise(const Type *value);
  void fulfillPromise(llvm::Value *promise, llvm::Value *value, const Type *type);
  void followPromise(llvm::Value *promise, llvm::Value *other);
  llvm::Value *emitPromiseNew(CallExpr *call);
  llvm::Value *emitPromiseStatic(CallExpr *call);
  llvm::Constant *resolverInfo(const Type *value);

private:
  Lowering &lowering_;
};

}
