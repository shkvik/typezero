#pragma once

#include "compiler/check/checker/checker.h"

namespace tz {

class SwitchChecker {
public:
  explicit SwitchChecker(Checker &owner);

  void checkSwitch(SwitchStmt *stmt);

private:
  Checker &checker;
};

}
