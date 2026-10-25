#pragma once

#include "compiler/check/sema/sema.h"

namespace tz {

// The members and methods of unions.
class UnionChecker {
public:
  explicit UnionChecker(Sema &sema);

  // `o.name` of a union of objects, whose object is checked: a field every
  // member has, of the union of their types.
  Expr *checkUnionMember(MemberExpr *member);
  // `s.label = v` of a union: a field of one type in every member, which
  // may be assigned to there.
  const Type *checkUnionFieldTarget(MemberExpr *member);
  // `s.area()` of a union of objects that no common type gives the method:
  // the method of each member, which must have one type.
  Expr *checkUnionMethodCall(CallExpr *call, MemberExpr *member);
  // A type that every member of the union `type` is declared to be and that
  // has a member `name` (a class they extend, an interface they implement):
  // the member is read through it. Null if none.
  const Type *commonBaseWith(const Type *type, llvm::StringRef name) const;
  // The values of the literal type a switch tests that no case handles;
  // nothing if it does not test one, or has a default.
  std::optional<std::vector<const Type *>> unhandledCases(const SwitchStmt *stmt) const;

private:
  Sema &sema_;
};

} // namespace tz
