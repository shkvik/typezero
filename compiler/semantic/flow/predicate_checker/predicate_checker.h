#pragma once

#include "compiler/result/result/result.h"
#include "compiler/semantic/flow/predicate_checker/predicate_checker_errors.h"
#include "compiler/semantic/semantic_state/semantic_state.h"

namespace tz {

class PredicateChecker {
public:
  using ResultExitNarrowed = Result<const Flow *, PredicateCheckerNotePredicateExitError>;

  explicit PredicateChecker(SemanticState &owner);

  const Type *resolveReturnType(FunctionDecl *fn, FuncSymbol *sym);
  static bool looksBoolean(const Expr *expr);
  void beginPredicate(FunctionDecl *fn);
  ResultExitNarrowed notePredicateExit(const Flow &whenTrue, const Flow &whenFalse);
  void endPredicate(FunctionDecl *fn);
  const Predicate *predicateOf(const FuncSymbol *fn) const;
  void applyPredicate(const CallExpr *call, Flow &whenTrue, Flow &whenFalse) const;
  const Type *filteredType(const Expr *callback, const Type *element) const;

private:
  using ResultNotAsserting = Result<const TypeRef *, PredicateCheckerNotAssertingError>;
  using ResultPredicateArg = Result<int, PredicateCheckerPredicateArgError>;

  static ResultPredicateArg predicateArg(const FunctionDecl *fn, const TypeRef &ref);
  ResultNotAsserting notAsserting(const FunctionDecl *fn, const TypeRef &ref);
  static const VarSymbol *argumentVariable(const Expr *expr);
  static std::optional<Linear> argumentTerm(const Expr *expr);

  SemanticState &state;
};

}
