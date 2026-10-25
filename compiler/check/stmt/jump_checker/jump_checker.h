#pragma once

#include "compiler/check/sema/sema.h"

namespace tz {

// break, continue and labels.
class JumpChecker {
public:
  explicit JumpChecker(Sema &sema);

  void checkLabeled(LabeledStmt *stmt);
  void checkJump(Stmt *stmt);
  // Enters a loop, switch or labeled block; a loop or switch takes the
  // labels in pendingLabels_.
  void pushTarget(Sema::JumpTarget::Kind kind);

private:
  Sema &sema_;
};

} // namespace tz
