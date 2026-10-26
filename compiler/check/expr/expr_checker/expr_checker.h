#pragma once

#include "compiler/check/checker/checker.h"

namespace tz {

class ExprChecker {
public:
  explicit ExprChecker(Checker &checker);

  Expr *checkExpr(Expr *expr, const Type *expected = nullptr, bool wraps = false);
  Expr *checkExprKind(Expr *expr, const Type *expected, bool wraps);
  Expr *checkSequence(SequenceExpr *seq, const Type *expected, bool wraps);
  Expr *checkOptionalChain(OptionalChainExpr *chain);
  Expr *chainLink(Expr *object);
  Expr *checkConditional(ConditionalExpr *cond, const Type *expected);
  Expr *checkCast(CastExpr *cast);

private:
  Checker &checker_;
};

}
