#pragma once

#include "compiler/lower/lowering/lowering.h"

namespace tz {

class ConsoleLowerer {
public:
  explicit ConsoleLowerer(Lowering &lowering);

  void emitConsoleLog(CallExpr *call);

private:
  Lowering &lowering_;
};

}
