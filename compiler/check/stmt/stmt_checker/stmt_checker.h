#pragma once

#include "compiler/check/checker/checker.h"

namespace tz {

class StmtChecker {
public:
  explicit StmtChecker(Checker &checker);

  void checkStmt(Stmt *stmt);
  void checkIf(IfStmt *stmt);
  void checkBody(Stmt *body);

private:
  Checker &checker_;
};

}
