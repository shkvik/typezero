#pragma once

#include "compiler/check/checker/checker.h"

namespace tz {

class NameChecker {
public:
  explicit NameChecker(Checker &owner);

  Expr *checkName(NameExpr *name);

private:
  Checker &checker;
};

}
