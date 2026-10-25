#pragma once

#include "compiler/lower/lowering/lowering.h"

namespace tz {

// spawnSync, execFileSync and execSync: a call of tz_child_run.
class ChildLowerer {
public:
  explicit ChildLowerer(Lowering &lowering);

  // spawnSync, execFileSync and execSync of node:child_process
  // (codegen_child.cpp).
  llvm::Value *emitChildBuiltin(CallExpr *call);
  // The maker of what they give, the result of spawnSync or an
  // ExecSyncError, from the TzSpawnParts of
  // runtime/library/child_process/child/child.c.
  llvm::Function *spawnMaker(const Type *type);
  llvm::StructType *spawnPartsType();
  // An owned value of `to` from what stream `stream` printed, in the parts
  // at `parts`: text, a Buffer, or undefined if it was no pipe. With
  // `retain`, the parts keep their reference.
  llvm::Value *spawnStream(llvm::Value *parts, unsigned stream, const Type *to, bool retain);

private:
  Lowering &lowering_;
  std::map<const Type *, llvm::Function *> spawnMakers_;
};

} // namespace tz
