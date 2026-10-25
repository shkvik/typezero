#pragma once

#include "compiler/lower/lowering/lowering.h"

namespace tz {

// console.log and its kin.
class ConsoleLowerer {
public:
  explicit ConsoleLowerer(Lowering &lowering);

  void emitConsoleLog(CallExpr *call);

private:
  Lowering &lowering_;
};

} // namespace tz
