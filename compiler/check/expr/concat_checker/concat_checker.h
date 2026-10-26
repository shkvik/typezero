#pragma once

#include "compiler/check/checker/checker.h"

namespace tz {

class ConcatChecker {
public:
  explicit ConcatChecker(Checker &checker);

  Expr *toStringOperand(Expr *expr);
  void reportMixedConcat(const BinaryExpr *root, const BinaryExpr *mixed);

private:
  Checker &checker_;
};

}
