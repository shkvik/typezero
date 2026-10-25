#pragma once

#include "compiler/check/sema/sema.h"

namespace tz {

// new: which class checks the construction.
class NewChecker {
public:
  explicit NewChecker(Sema &sema);

  Expr *checkNew(NewExpr *expr, const Type *expected);

private:
  Sema &sema_;
};

} // namespace tz
