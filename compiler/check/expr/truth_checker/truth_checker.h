#pragma once

#include "compiler/check/checker/checker.h"

namespace tz {

// What a condition may test, and what !, &&, || and Boolean(x) make of values.
class TruthChecker {
public:
  explicit TruthChecker(Checker &checker);

  // Checks a condition and computes the flow where it is true and where it is
  // false. The result is a boolean.
  Expr *checkCondition(Expr *cond, Checker::Flow &whenTrue, Checker::Flow &whenFalse);
  Expr *checkBranching(Expr *expr, Checker::Flow &whenTrue, Checker::Flow &whenFalse,
                       Checker::TruthUse use = Checker::TruthUse::Condition);
  // An operand of `&&` or `||` whose value is the result when it decides.
  Expr *checkTestedValue(Expr *expr, const Type *expected, Checker::Flow &whenTrue,
                         Checker::Flow &whenFalse, Checker::TruthUse use);
  // `a && b` and `a || b` as values. With `tested`, an enclosing `&&` or `||`
  // tests the result too.
  Expr *checkLogical(BinaryExpr *bin, const Type *expected, Checker::Flow &whenTrue,
                     Checker::Flow &whenFalse, bool tested, Checker::TruthUse use);
  // Reports a value whose truth cannot be tested; `other` is the right side
  // of `||` or the value of `||=`, for the hint.
  bool checkTruthy(const Expr *expr, Checker::TruthUse use, const Expr *other = nullptr);
  // `if (level = 0)`
  void reportAssignmentCondition(const AssignExpr *assign, Checker::TruthUse use);
  // `Boolean(x)`, which converts like a condition.
  Expr *checkBooleanCall(CallExpr *call, Checker::Flow &whenTrue, Checker::Flow &whenFalse);
  bool isBooleanCall(const Expr *expr) const;

private:
  Checker &checker_;
};

} // namespace tz
