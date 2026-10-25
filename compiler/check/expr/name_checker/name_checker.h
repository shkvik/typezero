#pragma once

#include "compiler/check/sema/sema.h"

namespace tz {

// Names in expressions: variables, functions, globals.
class NameChecker {
public:
  explicit NameChecker(Sema &sema);

  Expr *checkName(NameExpr *name);

private:
  Sema &sema_;
};

} // namespace tz
