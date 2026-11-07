#pragma once

#include "compiler/lower/lowering/lowering.h"
#include "compiler/lower/objects/object_maker/object_maker.h"
#include <map>

namespace tz {

class FsLowerer {
public:
  explicit FsLowerer(Lowering &owner);

  llvm::Value *emitFsCall(CallExpr *call);
  llvm::Value *emitFsResult(CallExpr *call);
  llvm::Function *objectMaker(const Type *type);
  llvm::Function *nativeErrorMaker();
  llvm::Function *errorMaker(const Type *error);

private:
  static const std::vector<MakerFields> &fsObjects();

  Lowering &lowering;
  std::map<const Type *, llvm::Function *> errorMakers;
};

}
