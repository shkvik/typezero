#pragma once

#include "compiler/check/checker/checker.h"

namespace tz {

// Fields of objects, Object.keys and its kin, for...in.
class ObjectChecker {
public:
  explicit ObjectChecker(Checker &checker);

  // Object.keys, Object.values and Object.entries, and for dictionaries
  // Object.fromEntries and Object.groupBy.
  Expr *checkObjectCall(CallExpr *call, MemberExpr *member, const Type *expected);
  // `for (const k in o)`: the loop over Object.keys(o).
  void checkForIn(ForOfStmt *stmt);
  // `object.name` of an object, whose object is checked.
  Expr *checkField(MemberExpr *member);
  // `object.name(args)` of an object: a call of the function in the field.
  Expr *checkFieldCall(CallExpr *call, MemberExpr *member);
  const Type *checkObjectFieldTarget(MemberExpr *member);
  // Why `from` does not fit an object type `to`: the fields that differ.
  // `conflict`: the object also goes where a field has another type, so
  // giving it a type would not help.
  std::string objectMismatch(const Type *from, const Type *to, bool conflict = false) const;

private:
  Checker &checker_;
};

} // namespace tz
