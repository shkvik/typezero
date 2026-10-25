#pragma once

#include "compiler/lower/lowering/lowering.h"

namespace tz {

// The functions of the runtime that programs call, and the helpers of reference counts.
class RuntimeFunctions {
public:
  explicit RuntimeFunctions(Lowering &lowering);

  // Module structure
  void declareRuntime();
  void defineRefCountHelpers();
  void countShared(llvm::IRBuilder<> &b, llvm::Value *obj, llvm::Value *count, bool retain,
                   llvm::BasicBlock *freeBlock, llvm::BasicBlock *doneBlock);

private:
  Lowering &lowering_;
};

} // namespace tz
