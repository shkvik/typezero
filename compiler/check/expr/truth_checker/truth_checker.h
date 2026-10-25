#pragma once

#include "compiler/check/sema/sema.h"

namespace tz {

// What a condition may test, and what !, &&, || and Boolean(x) make of values.
class TruthChecker {
public:
  explicit TruthChecker(Sema &sema);

  // Checks a condition and computes the flow where it is true and where it is
  // false. The result is a boolean.
  Expr *checkCondition(Expr *cond, Sema::Flow &whenTrue, Sema::Flow &whenFalse);
  Expr *checkBranching(Expr *expr, Sema::Flow &whenTrue, Sema::Flow &whenFalse,
                       Sema::TruthUse use = Sema::TruthUse::Condition);
  // An operand of `&&` or `||` whose value is the result when it decides.
  Expr *checkTestedValue(Expr *expr, const Type *expected, Sema::Flow &whenTrue, Sema::Flow &whenFalse,
                         Sema::TruthUse use);
  // `a && b` and `a || b` as values. With `tested`, an enclosing `&&` or `||`
  // tests the result too.
  Expr *checkLogical(BinaryExpr *bin, const Type *expected, Sema::Flow &whenTrue, Sema::Flow &whenFalse,
                     bool tested, Sema::TruthUse use);
  // Reports a value whose truth cannot be tested; `other` is the right side
  // of `||` or the value of `||=`, for the hint.
  bool checkTruthy(const Expr *expr, Sema::TruthUse use, const Expr *other = nullptr);
  // `if (level = 0)`
  void reportAssignmentCondition(const AssignExpr *assign, Sema::TruthUse use);
  // `Boolean(x)`, which converts like a condition.
  Expr *checkBooleanCall(CallExpr *call, Sema::Flow &whenTrue, Sema::Flow &whenFalse);
  bool isBooleanCall(const Expr *expr) const;

private:
  Sema &sema_;
};

} // namespace tz
