#pragma once

#include "compiler/check/checker/checker.h"

namespace tz {

class TargetChecker {
public:
  explicit TargetChecker(Checker &checker);

  const Type *checkAssignTarget(Expr *&target, bool compound);
  VarSymbol *checkVarTarget(NameExpr *name);

private:
  Checker &checker_;
};

}
