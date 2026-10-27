#pragma once

#include "compiler/check/checker/checker.h"

namespace tz {

class PredicateChecker {
public:
  explicit PredicateChecker(Checker &checker);

  const Type *resolveReturnType(FunctionDecl *fn, FuncSymbol *sym);
  static std::string assertsError(const std::string &shown, bool typed, const std::string &call);
  static bool looksBoolean(const Expr *expr);
  void beginPredicate(FunctionDecl *fn);
  void notePredicateExit(const Checker::Flow &whenTrue, const Checker::Flow &whenFalse,
                         llvm::SMLoc loc);
  void endPredicate(FunctionDecl *fn);
  const Checker::Predicate *predicateOf(const FuncSymbol *fn) const;
  void applyPredicate(const CallExpr *call, Checker::Flow &whenTrue,
                      Checker::Flow &whenFalse) const;
  const Type *filteredType(const Expr *callback, const Type *element) const;

private:
  Checker &checker_;
  mutable llvm::DenseMap<const FuncSymbol *, Checker::Predicate> predicates_;
  mutable llvm::SmallPtrSet<const FuncSymbol *, 8> summarizing_;
};

}
