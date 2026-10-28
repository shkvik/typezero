#pragma once

#include "compiler/lower/lowering/lowering.h"

namespace tz {

class LibraryCallLowerer {
public:
  explicit LibraryCallLowerer(Lowering &lowering);

  llvm::Value *emitLibBuiltin(CallExpr *call);

private:
  Lowering &lowering_;
};

}
