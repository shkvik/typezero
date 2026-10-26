#pragma once

#include "compiler/check/checker/checker.h"

namespace tz {

class SwitchChecker {
public:
  explicit SwitchChecker(Checker &checker);

  void checkSwitch(SwitchStmt *stmt);

private:
  Checker &checker_;
};

}
