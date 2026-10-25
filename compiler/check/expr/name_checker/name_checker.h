#pragma once

#include "compiler/check/checker/checker.h"

namespace tz {

// Names in expressions: variables, functions, globals.
class NameChecker {
public:
  explicit NameChecker(Checker &checker);

  Expr *checkName(NameExpr *name);

private:
  Checker &checker_;
};

} // namespace tz
