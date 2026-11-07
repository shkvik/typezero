#pragma once

#include "compiler/lower/lowering/lowering.h"

namespace tz {

class SwitchLowerer {
public:
  explicit SwitchLowerer(Lowering &owner);

  void emitSwitch(SwitchStmt *stmt);

private:
  Lowering &lowering;
};

}
