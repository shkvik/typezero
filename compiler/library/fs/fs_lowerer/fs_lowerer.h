#pragma once

#include "compiler/lower/lowering/lowering.h"
#include <map>

namespace tz {

class FsLowerer {
public:
  explicit FsLowerer(Lowering &lowering);

  llvm::Value *emitFsCall(CallExpr *call);
  llvm::Value *emitFsResult(CallExpr *call);
  llvm::Function *objectMaker(const Type *type);
  llvm::Function *nativeErrorMaker();
  llvm::Function *errorMaker(const Type *error);

private:
  Lowering &lowering_;
  std::map<const Type *, llvm::Function *> errorMakers_;
};

}
