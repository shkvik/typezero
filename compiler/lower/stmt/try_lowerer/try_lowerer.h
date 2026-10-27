#pragma once

#include "compiler/lower/lowering/lowering.h"

namespace tz {

class TryLowerer {
public:
  explicit TryLowerer(Lowering &lowering);

  void emitTry(TryStmt *stmt);
  void emitReturnOf(llvm::Value *value);
  void emitJumpTo(size_t target, bool isBreak);
  Lowering::TryContext *finallyBetween(size_t target);

private:
  Lowering &lowering_;
};

}
