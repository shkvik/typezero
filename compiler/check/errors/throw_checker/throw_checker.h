#pragma once

#include "compiler/check/checker/checker.h"

namespace tz {

class ThrowChecker {
public:
  explicit ThrowChecker(Checker &checker);

  void checkThrow(ThrowStmt *stmt);
  void checkTry(TryStmt *stmt);
  Checker::Flow withoutChanges(const Checker::Flow &flow, const Checker::LoopScan &scan);

private:
  Checker &checker_;
};

}
