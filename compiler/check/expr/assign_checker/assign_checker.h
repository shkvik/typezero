#pragma once

#include "compiler/check/checker/checker.h"

namespace tz {

// Assignments: plain, compound and logical, ++ and --.
class AssignChecker {
public:
  explicit AssignChecker(Checker &checker);

  Expr *checkAssign(AssignExpr *assign);
  Expr *checkLogicalAssign(AssignExpr *assign);
  Expr *checkUpdate(UpdateExpr *update);
  // `o.f ??= v`: `o.f ?? (o.f = v)`, when the object can be read twice.
  // (`||=` and `&&=` on fields go through checkLogicalAssign.)
  Expr *checkFieldLogicalAssign(AssignExpr *assign);

private:
  Checker &checker_;
  bool logicalAssign_ = false; // the next assignment checked is that of `o.x ??= v`
};

} // namespace tz
