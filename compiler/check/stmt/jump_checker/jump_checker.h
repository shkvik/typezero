#pragma once

#include "compiler/check/checker/checker.h"

namespace tz {

// break, continue and labels.
class JumpChecker {
public:
  explicit JumpChecker(Checker &checker);

  void checkLabeled(LabeledStmt *stmt);
  void checkJump(Stmt *stmt);
  // Enters a loop, switch or labeled block; a loop or switch takes the
  // labels in pendingLabels_.
  void pushTarget(Checker::JumpTarget::Kind kind);

private:
  Checker &checker_;
};

} // namespace tz
