#pragma once

#include "compiler/lower/lowering/lowering.h"

namespace tz {

class FsLowerer {
public:
  explicit FsLowerer(Lowering &lowering);

  llvm::Value *emitFsCall(CallExpr *call);
  llvm::Function *objectMaker(const Type *type);
  llvm::Function *systemErrorMaker();

private:
  Lowering &lowering_;
};

}
