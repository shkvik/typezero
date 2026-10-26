#pragma once

#include "compiler/lower/lowering/lowering.h"

namespace tz {

class OsLowerer {
public:
  explicit OsLowerer(Lowering &lowering);

  llvm::Value *emitOsCall(CallExpr *call);

private:
  Lowering &lowering_;
};

}
