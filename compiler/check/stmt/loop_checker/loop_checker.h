#pragma once

#include "compiler/check/checker/checker.h"

namespace tz {

// while, do while, for and for...of.
class LoopChecker {
public:
  explicit LoopChecker(Checker &checker);

  void checkWhile(WhileStmt *stmt);
  void checkDoWhile(DoWhileStmt *stmt);
  void checkFor(ForStmt *stmt);
  void checkForOf(ForOfStmt *stmt);
  void checkLoopBody(Stmt *body);

private:
  Checker &checker_;
};

} // namespace tz
