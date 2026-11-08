#pragma once

#include "compiler/result/result/result.h"
#include "compiler/semantic/bodies/expr/expr_checker/expr_checker_errors.h"
#include "compiler/semantic/semantic_state/semantic_state.h"

namespace tz {

class ExprChecker {
public:
  explicit ExprChecker(SemanticState &owner);

  Expr *checkExpr(Expr *expr, const Type *expected = nullptr, bool wraps = false);
  Expr *checkExprKind(Expr *expr, const Type *expected, bool wraps);
  Expr *checkSequence(SequenceExpr *seq, const Type *expected, bool wraps);
  Expr *checkOptionalChain(OptionalChainExpr *chain);
  Expr *chainLink(Expr *object);

private:
  using ResultBranchesMatch = Result<Expr *, ExprCheckerCheckConditionalError>;
  using ResultCastSupported = Result<Expr *, ExprCheckerCheckCastError>;

  ResultCastSupported checkCast(CastExpr *cast);
  ResultBranchesMatch checkConditional(ConditionalExpr *cond, const Type *expected);

  SemanticState &state;
};

}
