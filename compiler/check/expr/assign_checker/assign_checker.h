#pragma once

#include "compiler/check/sema/sema.h"

namespace tz {

// Assignments: plain, compound and logical, ++ and --.
class AssignChecker {
public:
  explicit AssignChecker(Sema &sema);

  Expr *checkAssign(AssignExpr *assign);
  Expr *checkLogicalAssign(AssignExpr *assign);
  Expr *checkUpdate(UpdateExpr *update);
  // `o.f ??= v`: `o.f ?? (o.f = v)`, when the object can be read twice.
  // (`||=` and `&&=` on fields go through checkLogicalAssign.)
  Expr *checkFieldLogicalAssign(AssignExpr *assign);

private:
  Sema &sema_;
  bool logicalAssign_ = false; // the next assignment checked is that of `o.x ??= v`
};

} // namespace tz
