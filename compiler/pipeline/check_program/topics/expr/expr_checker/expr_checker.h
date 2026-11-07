#pragma once

#include "compiler/pipeline/check_program/check_state/check_state.h"
#include "compiler/pipeline/check_program/topics/expr/expr_checker/expr_checker_errors.h"
#include "compiler/result/result/result.h"

namespace tz {

class ExprChecker {
public:
  explicit ExprChecker(CheckState &owner);

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

  CheckState &state;
};

}
