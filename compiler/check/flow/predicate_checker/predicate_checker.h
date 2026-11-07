#pragma once

#include "compiler/check/checker/checker.h"
#include "compiler/check/flow/predicate_checker/predicate_checker_errors.h"
#include "compiler/result/result/result.h"

namespace tz {

class PredicateChecker {
public:
  using ResultExitNarrowed = Result<const Checker::Flow *, PredicateCheckerNotePredicateExitError>;

  explicit PredicateChecker(Checker &owner);

  const Type *resolveReturnType(FunctionDecl *fn, FuncSymbol *sym);
  static bool looksBoolean(const Expr *expr);
  void beginPredicate(FunctionDecl *fn);
  ResultExitNarrowed notePredicateExit(const Checker::Flow &whenTrue,
                                       const Checker::Flow &whenFalse);
  void endPredicate(FunctionDecl *fn);
  const Checker::Predicate *predicateOf(const FuncSymbol *fn) const;
  void applyPredicate(const CallExpr *call, Checker::Flow &whenTrue,
                      Checker::Flow &whenFalse) const;
  const Type *filteredType(const Expr *callback, const Type *element) const;

private:
  using ResultNotAsserting = Result<const TypeRef *, PredicateCheckerNotAssertingError>;
  using ResultPredicateArg = Result<int, PredicateCheckerPredicateArgError>;

  static ResultPredicateArg predicateArg(const FunctionDecl *fn, const TypeRef &ref);
  ResultNotAsserting notAsserting(const FunctionDecl *fn, const TypeRef &ref);
  static const VarSymbol *argumentVariable(const Expr *expr);
  static std::optional<Linear> argumentTerm(const Expr *expr);

  Checker &checker;
  mutable llvm::DenseMap<const FuncSymbol *, Checker::Predicate> predicates;
  mutable llvm::SmallPtrSet<const FuncSymbol *, 8> summarizing;
};

}
