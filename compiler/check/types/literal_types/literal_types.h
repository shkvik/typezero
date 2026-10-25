#pragma once

#include "compiler/check/checker/checker.h"

namespace tz {

// Literal types and the member of a union a value is.
class LiteralTypes {
public:
  explicit LiteralTypes(Checker &checker);

  // Unions and literal types (sema_union.cpp, docs/objects.md section 7).
  //
  // The member of the union `to` that a value of `from` (not a union) goes
  // into: the same type, its base for a literal, or else the first member
  // it converts to; null if none.
  static const Type *unionMemberFor(const Type *from, const Type *to);
  // What unify gives, or else the union of both where one can hold them
  // (`flag ? 1 : "one"`); null if neither.
  static const Type *unionUnify(const Type *a, const Type *b);
  // The literal type of a literal written where `expected` is wanted, if
  // literal types of its kind are expected there (`const d: Dir = "up"`);
  // null where it has its base type.
  const Type *stringLiteralFor(const Type *expected, const std::u16string &text);
  const Type *boolLiteralFor(const Type *expected, bool value);
  const Type *numberLiteralFor(const Type *expected, double value, bool fraction);
  // The number type a number literal takes where the union `expected` is
  // wanted: its member int, long, double or decimal; null if none.
  static const Type *numberMemberOf(const Type *expected, bool fraction);
  // The member of the union `target` that an object literal is written for:
  // the object type with the fields it names, told apart by the tags it
  // gives (`kind: "circle"`); null if none or several fit.
  const Type *literalMember(const ObjectLitExpr *lit, const Type *target) const;
  // The type of `var` in `flow`: narrowed, or holding a value, or declared.
  const Type *typeIn(const Checker::Flow &flow, const VarSymbol *var) const;
  // What a literal written where `expected` is expected looks at: of a
  // conditional type that waits, the union of its branches, so that `return
  // "text"` takes the literal type a branch has.
  const Type *literalExpectation(const Type *expected, unsigned depth = 0);

private:
  Checker &checker_;
};

} // namespace tz
