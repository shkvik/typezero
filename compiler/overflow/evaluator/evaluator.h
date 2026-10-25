#pragma once

#include "compiler/overflow/overflow_check/overflow_check.h"

namespace tz {

// Expressions evaluated to ranges.
class Evaluator {
public:
  explicit Evaluator(OverflowCheck &check);

  Val eval(const Expr *expr, State &st, bool wraps = false);
  Val evalName(const NameExpr *name, State &st);
  Val evalMember(const MemberExpr *member, State &st);
  Val evalIndex(const IndexExpr *index, State &st);
  Val evalUnary(const UnaryExpr *unary, State &st, bool wraps);
  Val evalBinary(const BinaryExpr *bin, State &st, bool wraps);
  Val evalCast(const CastExpr *conv, State &st, bool wraps);
  // An operation of `type` that may overflow, at `site`.
  Num arithmetic(const Expr *site, BinaryOp op, const Num &l, const Num &r, const Type *type,
                 bool wraps, const Expr *lhs, const Expr *rhs, const VarSymbol *accum);
  Num bitwise(BinaryOp op, const Num &l, const Num &r, const Type *type) const;

private:
  OverflowCheck &check_;
};

} // namespace tz
