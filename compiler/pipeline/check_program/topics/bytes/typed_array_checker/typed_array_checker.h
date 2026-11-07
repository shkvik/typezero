#pragma once

#include "compiler/pipeline/check_program/check_state/check_state.h"

namespace tz {

class TypedArrayChecker {
public:
  explicit TypedArrayChecker(CheckState &owner);

  Expr *checkTypedArrayMethod(CallExpr *call, MemberExpr *member, const Type *expected);
  Expr *checkDataViewMethod(CallExpr *call, MemberExpr *member);

private:
  static std::string rangeText(int64_t lo, int64_t hi);

  CheckState &state;

  void checkSetRange(const Expr *target, const Expr *source, const Expr *offset);
};

}
