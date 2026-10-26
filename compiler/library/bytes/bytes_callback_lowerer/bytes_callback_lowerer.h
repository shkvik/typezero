#pragma once

#include "compiler/lower/lowering/lowering.h"

namespace tz {

class BytesCallbackLowerer {
public:
  explicit BytesCallbackLowerer(Lowering &lowering);

  llvm::Value *emitBytesCallbackMethod(CallExpr *call);

private:
  Lowering &lowering_;
};

}
