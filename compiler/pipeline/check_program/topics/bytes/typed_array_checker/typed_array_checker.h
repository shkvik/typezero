#pragma once

#include "compiler/pipeline/check_program/check_state/check_state.h"
#include "compiler/pipeline/check_program/topics/bytes/typed_array_checker/typed_array_checker_errors.h"
#include "compiler/result/result/result.h"

namespace tz {

class TypedArrayChecker {
public:
  explicit TypedArrayChecker(CheckState &owner);

  Expr *checkTypedArrayMethod(CallExpr *call, MemberExpr *member, const Type *expected);
  Expr *checkDataViewMethod(CallExpr *call, MemberExpr *member);

private:
  using ResultSetSourceOk = Result<const Expr *, TypedArrayCheckerSetSourceError>;

  static ResultSetSourceOk setSource(const Expr *source);

  CheckState &state;

  void checkSetRange(const Expr *target, const Expr *source, const Expr *offset);
};

}
