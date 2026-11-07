#pragma once

#include "compiler/check/checker/checker.h"

namespace tz {

class TypedArrayChecker {
public:
  explicit TypedArrayChecker(Checker &owner);

  Expr *checkTypedArrayMethod(CallExpr *call, MemberExpr *member, const Type *expected);
  Expr *checkDataViewMethod(CallExpr *call, MemberExpr *member);

private:
  static std::string rangeText(int64_t lo, int64_t hi);

  Checker &checker;

  void checkSetRange(const Expr *target, const Expr *source, const Expr *offset);
};

}
