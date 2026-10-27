#pragma once

#include "compiler/lower/lowering/lowering.h"

namespace tz {

class ExitLowerer {
public:
  explicit ExitLowerer(Lowering &lowering);

  void emitReturnOf(llvm::Value *value);
  void emitJumpTo(size_t target, bool isBreak);

private:
  Lowering &lowering_;
};

}
