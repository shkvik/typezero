#pragma once

#include "compiler/lower/codegen/codegen.h"

namespace tz {

// Promises: their layout, new Promise, resolve and reject.
class PromiseLowerer {
public:
  explicit PromiseLowerer(CodeGen &codegen);

  // The bytes the value of a Promise<value> takes, a multiple of 8.
  uint64_t promiseValueSize(const Type *value);
  llvm::Value *promiseValueAddress(llvm::Value *promise);
  llvm::Value *promiseField(llvm::Value *promise, unsigned field);
  // The layout of the promises of a value of type `value`: the drop function
  // of the runtime, and the value as its one field.
  llvm::Constant *promiseLayout(const Type *value);
  llvm::Value *newPromise(const Type *value);
  // Settles `promise`: with the owned `value` (null if it holds nothing),
  // with an owned error, or as the owned promise `other` does.
  void fulfillPromise(llvm::Value *promise, llvm::Value *value, const Type *type);
  void rejectPromise(llvm::Value *promise, llvm::Value *error);
  void followPromise(llvm::Value *promise, llvm::Value *other);
  llvm::Value *emitPromiseNew(CallExpr *call);
  llvm::Value *emitPromiseStatic(CallExpr *call);
  llvm::Constant *resolverInfo(const Type *value);
  llvm::Constant *rejecterInfo();

private:
  CodeGen &codegen_;
  llvm::Constant *rejecterInfo_ = nullptr;
};

} // namespace tz
