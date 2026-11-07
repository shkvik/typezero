#pragma once

#include "compiler/check/checker/checker.h"

namespace tz {

class ArrayMethods {
public:
  explicit ArrayMethods(Checker &owner);

  Expr *checkArrayMethod(CallExpr *call, MemberExpr *member, const Type *expected);

private:
  static const VarSymbol *arrayVariable(const Expr *object);

  Checker &checker;
};

}
