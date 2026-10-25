#pragma once

#include "compiler/check/sema/sema.h"

namespace tz {

// Which class an expression goes to; sequences, optional chains, conditionals and casts.
class ExprChecker {
public:
  explicit ExprChecker(Sema &sema);

  // Expressions. Each returns the expression to use in place of the argument,
  // which differs when a conversion was inserted. `wraps`: only the low bits
  // of the value matter, as in an operand of `|` (see foldBinary).
  Expr *checkExpr(Expr *expr, const Type *expected = nullptr, bool wraps = false);
  Expr *checkExprKind(Expr *expr, const Type *expected, bool wraps);
  Expr *checkSequence(SequenceExpr *seq, const Type *expected, bool wraps);
  Expr *checkOptionalChain(OptionalChainExpr *chain);
  // The object of a link `object?.x` of an optional chain: if it may be
  // undefined, a conversion to its value that ends the chain when it is
  // undefined (see OptionalChainExpr).
  Expr *chainLink(Expr *object);
  Expr *checkConditional(ConditionalExpr *cond, const Type *expected);
  Expr *checkCast(CastExpr *cast);

private:
  Sema &sema_;
};

} // namespace tz
