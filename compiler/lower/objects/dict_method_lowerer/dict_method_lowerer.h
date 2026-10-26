#pragma once

#include "compiler/lower/lowering/lowering.h"

namespace tz {

class DictMethodLowerer {
public:
  explicit DictMethodLowerer(Lowering &lowering);

  llvm::Value *emitDictCall(CallExpr *call);
  llvm::Value *emitDictNew(CallExpr *call);
  llvm::Value *emitGroupBy(CallExpr *call);
  llvm::Value *emitDictObjectCall(CallExpr *call);

private:
  Lowering &lowering_;
};

}
