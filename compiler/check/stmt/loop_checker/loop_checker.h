#pragma once

#include "compiler/check/checker/checker.h"

namespace tz {

class LoopChecker {
public:
  explicit LoopChecker(Checker &owner);

  void checkWhile(WhileStmt *stmt);
  void checkDoWhile(DoWhileStmt *stmt);
  void checkFor(ForStmt *stmt);
  void checkForOf(ForOfStmt *stmt);
  void checkLoopBody(Stmt *body, bool awaits);

private:
  Checker &checker;
};

}
