#pragma once

#include "compiler/check/checker/checker.h"

namespace tz {

// What may be assigned to.
class TargetChecker {
public:
  explicit TargetChecker(Checker &checker);

  // Assignment targets: a mutable variable, an array element or the length of
  // an array. Returns the type the target holds.
  // A target `o[key]` becomes the field it names.
  const Type *checkAssignTarget(Expr *&target, bool compound);
  VarSymbol *checkVarTarget(NameExpr *name);

private:
  Checker &checker_;
};

} // namespace tz
