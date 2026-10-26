#pragma once

#include "compiler/lower/lowering/lowering.h"

namespace tz {

class FsPromisesLowerer {
public:
  explicit FsPromisesLowerer(Lowering &lowering);

  llvm::Value *emitFsPromise(CallExpr *call);
  llvm::Value *emitReadlineBuiltin(CallExpr *call);
  llvm::Value *emitAsyncIoBuiltin(CallExpr *call);

private:
  Lowering &lowering_;
};

}
