#pragma once

#include "compiler/result/result/result.h"
#include "compiler/semantic/library/weakref/weak_ref_checker/weak_ref_checker_errors.h"
#include "compiler/semantic/semantic_state/semantic_state.h"

namespace tz {

class WeakRefChecker {
public:
  explicit WeakRefChecker(SemanticState &owner);

  const Type *resolveWeakRefType(const TypeRef &ref);
  Expr *checkWeakRefNew(NewExpr *expr, const Type *expected);
  Expr *checkWeakRefMethod(CallExpr *call, MemberExpr *member);

private:
  using ResultWeakTargetValid = Result<const Type *, WeakRefCheckerWeakTargetError>;
  using ResultNewTakesOneArgs = Result<const NewExpr *, WeakRefCheckerNewArgsError>;
  using ResultNewTypeArgsFits = Result<const NewExpr *, WeakRefCheckerNewTypeArgsError>;
  using ResultTargetIsWritten = Result<const TypeRef *, WeakRefCheckerTargetWrittenError>;
  using ResultMethodIsDerefed = Result<const MemberExpr *, WeakRefCheckerMethodNameError>;

  static ResultNewTakesOneArgs newArgs(const NewExpr *expr);
  static ResultWeakTargetValid weakTarget(const Type *target);
  static ResultNewTypeArgsFits newTypeArgs(const NewExpr *expr);
  static ResultTargetIsWritten targetWritten(const TypeRef &ref);
  static ResultMethodIsDerefed methodName(const MemberExpr *member, const Type *type);

  SemanticState &state;
};

}
