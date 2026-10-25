#pragma once

#include "compiler/check/checker/checker.h"

namespace tz {

// switch.
class SwitchChecker {
public:
  explicit SwitchChecker(Checker &checker);

  void checkSwitch(SwitchStmt *stmt);

private:
  Checker &checker_;
};

} // namespace tz
