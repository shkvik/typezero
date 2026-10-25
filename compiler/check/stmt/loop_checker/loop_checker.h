#pragma once

#include "compiler/check/sema/sema.h"

namespace tz {

// while, do while, for and for...of.
class LoopChecker {
public:
  explicit LoopChecker(Sema &sema);

  void checkWhile(WhileStmt *stmt);
  void checkDoWhile(DoWhileStmt *stmt);
  void checkFor(ForStmt *stmt);
  void checkForOf(ForOfStmt *stmt);
  void checkLoopBody(Stmt *body);

private:
  Sema &sema_;
};

} // namespace tz
