#pragma once

#include "compiler/check/checker/checker.h"

namespace tz {

class TruthChecker {
public:
  explicit TruthChecker(Checker &checker);

  Expr *checkCondition(Expr *cond, Checker::Flow &whenTrue, Checker::Flow &whenFalse);
  Expr *checkBranching(Expr *expr, Checker::Flow &whenTrue, Checker::Flow &whenFalse,
                       Checker::TruthUse use = Checker::TruthUse::Condition);
  Expr *checkTestedValue(Expr *expr, const Type *expected, Checker::Flow &whenTrue,
                         Checker::Flow &whenFalse, Checker::TruthUse use);
  Expr *checkLogical(BinaryExpr *bin, const Type *expected, Checker::Flow &whenTrue,
                     Checker::Flow &whenFalse, bool tested, Checker::TruthUse use);
  bool checkTruthy(const Expr *expr, Checker::TruthUse use, const Expr *other = nullptr);
  void reportAssignmentCondition(const AssignExpr *assign, Checker::TruthUse use);
  Expr *checkBooleanCall(CallExpr *call, Checker::Flow &whenTrue, Checker::Flow &whenFalse);
  bool isBooleanCall(const Expr *expr) const;

private:
  Checker &checker_;
};

}
