#pragma once

#include "compiler/check/checker/checker.h"

namespace tz {

// Json and null (docs/modules.md, section 5).
class JsonChecker {
public:
  explicit JsonChecker(Checker &checker);

  // Json (sema_json.cpp, docs/modules.md section 5). A Json is read as
  // JavaScript reads what JSON.parse gives: `j.name`, `j["name"]` and `j[0]`
  // are `Json | undefined` (from undefined or null the program stops, where
  // JavaScript throws), and checks narrow a Json variable: `typeof j ==
  // "string"`, `Array.isArray(j)`, `j != null`. A string, a number, a boolean
  // and a Json[] become a Json where one is expected, and so do object and
  // array literals; `null` exists only there.
  //
  // A Json, or an optional or an array of them.
  static bool holdsJson(const Type *type);
  // The literal `null`, before or after it is checked.
  static bool isNullName(const Expr *expr);
  // `null` where a Json is expected; null for anything else.
  Expr *checkNull(NameExpr *name, const Type *expected);
  // `j.name`, `j[key]` and `j[i]` of a Json, whose object is checked.
  Expr *checkJsonMember(MemberExpr *member);
  Expr *checkJsonIndex(IndexExpr *index);
  // `j.name = v` and `j[key] = v`: the type assigned, `Json | undefined`
  // (undefined removes the key).
  const Type *checkJsonTarget(Expr *target);
  // `j.map(f)`: a Json has no methods before it is narrowed.
  Expr *reportJsonMethod(CallExpr *call, MemberExpr *member);
  // `{ ... }` and `[...]` where a Json is expected.
  Expr *checkJsonObjectLit(ObjectLitExpr *lit);
  Expr *checkJsonArrayLit(ArrayLitExpr *lit);
  // `x == null`, before its sides are checked; null if neither is `null`.
  Expr *checkNullEquality(BinaryExpr *bin);
  // `==` and `!=` with a Json side, both sides checked.
  Expr *checkJsonEquality(BinaryExpr *bin);
  // What `typeof j == "string"`, `Array.isArray(j)` and `j != null` say
  // about a Json variable j.
  void addJsonFacts(const Expr *cond, Checker::Flow &whenTrue, Checker::Flow &whenFalse) const;
  Expr *checkArrayIsArray(CallExpr *call, MemberExpr *member);

private:
  Checker &checker_;
};

} // namespace tz
