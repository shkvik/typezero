#pragma once

#include "compiler/overflow/overflow_check/overflow_check.h"

namespace tz {

// Statements run abstractly.
class Executor {
public:
  explicit Executor(OverflowCheck &check);

  void exec(const Stmt *stmt, State &st);
  void execLabeled(const LabeledStmt *stmt, State &st);
  void execTry(const TryStmt *stmt, State &st);
  void execSwitch(const SwitchStmt *stmt, State &st);
  void execReturn(const ReturnStmt *ret, State &st);
  void jump(const Stmt *stmt, llvm::StringRef label, bool isBreak, State &st);

private:
  OverflowCheck &check_;
};

} // namespace tz
