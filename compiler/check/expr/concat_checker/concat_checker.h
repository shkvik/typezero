#pragma once

#include "compiler/check/checker/checker.h"

namespace tz {

// The conversions of + to strings, and the concatenations of numbers and strings.
class ConcatChecker {
public:
  explicit ConcatChecker(Checker &checker);

  // Converts a value to a string for `+` and template literals.
  Expr *toStringOperand(Expr *expr);
  // Reports a number or a boolean joined with a string by `+` in the chain
  // `a + b + c` that ends at `root`; `mixed` is the first `+` that does it.
  void reportMixedConcat(const BinaryExpr *root, const BinaryExpr *mixed);

private:
  Checker &checker_;
};

} // namespace tz
