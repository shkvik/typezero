#pragma once

#include "compiler/check/checker/checker.h"

namespace tz {

class JsonChecker {
public:
  explicit JsonChecker(Checker &checker);

  static bool holdsJson(const Type *type);
  static bool isNullName(const Expr *expr);
  Expr *checkNull(NameExpr *name, const Type *expected);
  Expr *checkJsonMember(MemberExpr *member);
  Expr *checkJsonIndex(IndexExpr *index);
  const Type *checkJsonTarget(Expr *target);
  Expr *reportJsonMethod(CallExpr *call, MemberExpr *member);
  Expr *checkJsonObjectLit(ObjectLitExpr *lit);
  Expr *checkJsonArrayLit(ArrayLitExpr *lit);
  Expr *checkNullEquality(BinaryExpr *bin);
  Expr *checkJsonEquality(BinaryExpr *bin);
  void addJsonFacts(const Expr *cond, Checker::Flow &whenTrue, Checker::Flow &whenFalse) const;
  Expr *checkArrayIsArray(CallExpr *call, MemberExpr *member);

private:
  Checker &checker_;
};

}
