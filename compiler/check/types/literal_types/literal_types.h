#pragma once

#include "compiler/check/checker/checker.h"

namespace tz {

class LiteralTypes {
public:
  explicit LiteralTypes(Checker &checker);

  static const Type *unionMemberFor(const Type *from, const Type *to);
  static const Type *unionUnify(const Type *a, const Type *b);
  const Type *stringLiteralFor(const Type *expected, const std::u16string &text);
  const Type *boolLiteralFor(const Type *expected, bool value);
  const Type *numberLiteralFor(const Type *expected, double value, bool fraction);
  static const Type *numberMemberOf(const Type *expected, bool fraction);
  const Type *literalMember(const ObjectLitExpr *lit, const Type *target) const;
  const Type *typeIn(const Checker::Flow &flow, const VarSymbol *var) const;
  const Type *literalExpectation(const Type *expected, unsigned depth = 0);

private:
  Checker &checker_;
};

}
