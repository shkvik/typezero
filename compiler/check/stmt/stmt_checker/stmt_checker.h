#pragma once

#include "compiler/check/sema/sema.h"

namespace tz {

// Which class a statement goes to; blocks and if.
class StmtChecker {
public:
  explicit StmtChecker(Sema &sema);

  void checkStmt(Stmt *stmt);
  void checkIf(IfStmt *stmt);
  void checkBody(Stmt *body);

private:
  Sema &sema_;
};

} // namespace tz
