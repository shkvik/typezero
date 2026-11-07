#pragma once

#include "compiler/check/checker/checker.h"

namespace tz {

class JsonChecker {
public:
  explicit JsonChecker(Checker &owner);

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
  std::optional<Checker::FieldPath> jsonField(const Expr *expr) const;
  static void narrowJsonField(Checker::Flow &flow, const Checker::FieldPath &path,
                              const Type *type);
  Expr *checkArrayIsArray(CallExpr *call, MemberExpr *member);
  Expr *checkJsonIsJson(CallExpr *call, MemberExpr *member);
  void checkRedundantTests(const BinaryExpr *bin);

private:
  static const VarSymbol *jsonVariable(const Expr *expr);
  static bool nullName(const Expr *expr);
  static bool isNothing(const Expr *expr);
  static const Expr *bare(const Expr *expr);
  static bool sameRef(const Expr *a, const Expr *b);
  static std::string refText(const Expr *expr);

  Checker &checker;
};

}
