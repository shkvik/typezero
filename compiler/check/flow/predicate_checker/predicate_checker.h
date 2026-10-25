#pragma once

#include "compiler/check/sema/sema.h"

namespace tz {

// Functions that test: x is T, asserts, and the facts of boolean functions
// (docs/narrowing.md).
class PredicateChecker {
public:
  explicit PredicateChecker(Sema &sema);

  // The return type of `fn`, written: a predicate `x is T` is a boolean
  // (void with asserts), which `sym` records.
  const Type *resolveReturnType(FunctionDecl *fn, FuncSymbol *sym);
  // An expression whose value is surely a boolean, by its form.
  static bool looksBoolean(const Expr *expr);
  // Around the check of a body: the summary of its returns.
  void beginPredicate(FunctionDecl *fn);
  void notePredicateExit(const Sema::Flow &whenTrue, const Sema::Flow &whenFalse, llvm::SMLoc loc);
  void endPredicate(FunctionDecl *fn);
  // The summary of `fn`, from its body; null if it has none.
  const Sema::Predicate *predicateOf(const FuncSymbol *fn) const;
  // What a call of a function that tests says, in a condition.
  void applyPredicate(const CallExpr *call, Sema::Flow &whenTrue, Sema::Flow &whenFalse) const;
  // The element type that `filter` with the callback keeps, narrower than
  // `element`; null if none.
  const Type *filteredType(const Expr *callback, const Type *element) const;

private:
  Sema &sema_;
  mutable llvm::DenseMap<const FuncSymbol *, Sema::Predicate> predicates_;
  mutable llvm::SmallPtrSet<const FuncSymbol *, 8> summarizing_;
};

} // namespace tz
