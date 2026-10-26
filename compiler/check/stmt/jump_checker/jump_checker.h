#pragma once

#include "compiler/check/checker/checker.h"

namespace tz {

class JumpChecker {
public:
  explicit JumpChecker(Checker &checker);

  void checkLabeled(LabeledStmt *stmt);
  void checkJump(Stmt *stmt);
  void pushTarget(Checker::JumpTarget::Kind kind);

private:
  Checker &checker_;
};

}
