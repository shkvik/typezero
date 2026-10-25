#pragma once

#include "compiler/check/checker/checker.h"

namespace tz {

// The helpers of iterators (ES2025), Iterator.from and Array.from.
class IteratorMethods {
public:
  explicit IteratorMethods(Checker &checker);

  // Marks the stages of the chain `expr` before it is checked: the consumer
  // that takes it apart runs it as one loop.
  void markFused(Expr *expr) const;
  // The methods of an iterator: next() and the helpers.
  Expr *checkIteratorMethod(CallExpr *call, MemberExpr *member);
  // keys(), values() and entries() of an array.
  Expr *checkArrayIterator(CallExpr *call, MemberExpr *member);
  // Iterator.from(x) and Array.from(x, f?).
  Expr *checkIteratorFrom(CallExpr *call, MemberExpr *member);
  Expr *checkArrayFrom(CallExpr *call, MemberExpr *member);

private:
  Checker &checker_;
};

} // namespace tz
