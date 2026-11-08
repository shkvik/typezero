#pragma once

#include "compiler/result/result/result.h"
#include "compiler/semantic/library/bytes/typed_array_checker/typed_array_checker_errors.h"
#include "compiler/semantic/semantic_state/semantic_state.h"

namespace tz {

class TypedArrayChecker {
public:
  explicit TypedArrayChecker(SemanticState &owner);

  Expr *checkTypedArrayMethod(CallExpr *call, MemberExpr *member, const Type *expected);
  Expr *checkDataViewMethod(CallExpr *call, MemberExpr *member);

private:
  using ResultSetSourceOk = Result<const Expr *, TypedArrayCheckerSetSourceError>;

  static ResultSetSourceOk setSource(const Expr *source);

  SemanticState &state;

  void checkSetRange(const Expr *target, const Expr *source, const Expr *offset);
};

}
