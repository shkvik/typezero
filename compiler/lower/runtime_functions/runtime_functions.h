#pragma once

#include "compiler/lower/lowering/lowering.h"

namespace tz {

class RuntimeFunctions {
public:
  explicit RuntimeFunctions(Lowering &lowering);

  void declareRuntime();
  void defineRefCountHelpers();
  void countShared(llvm::IRBuilder<> &b, llvm::Value *obj, llvm::Value *count, bool retain,
                   llvm::BasicBlock *freeBlock, llvm::BasicBlock *doneBlock);

private:
  Lowering &lowering_;
};

}
