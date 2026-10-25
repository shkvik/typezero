#pragma once

#include "compiler/lower/lowering/lowering.h"

namespace tz {

// try, catch and finally.
class TryLowerer {
public:
  explicit TryLowerer(Lowering &lowering);

  void emitTry(TryStmt *stmt);
  // `return` and the jumps that may leave a `try` with a `finally`: they run
  // it first. emitReturnOf returns `value` (owned, of the function's return
  // type; null for none).
  void emitReturnOf(llvm::Value *value);
  void emitJumpTo(size_t target, bool isBreak);
  Lowering::TryContext *finallyBetween(size_t target);

private:
  Lowering &lowering_;
};

} // namespace tz
