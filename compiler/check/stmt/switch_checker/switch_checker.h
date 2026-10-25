#pragma once

#include "compiler/check/sema/sema.h"

namespace tz {

// switch.
class SwitchChecker {
public:
  explicit SwitchChecker(Sema &sema);

  void checkSwitch(SwitchStmt *stmt);

private:
  Sema &sema_;
};

} // namespace tz
