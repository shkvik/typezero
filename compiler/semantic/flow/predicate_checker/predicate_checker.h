#pragma once

#include "compiler/result/result/result.h"
#include "compiler/semantic/flow/predicate_checker/predicate_checker_errors.h"
#include "compiler/semantic/semantic_state/semantic_state.h"

namespace tz {

class SemanticParts;
class FlowChecker;
class LiteralTypes;
class TypeResolver;
class ReturnInferrer;
class FunctionChecker;

class PredicateChecker {
public:
  using ResultExitNarrowed = Result<const Flow *, PredicateCheckerNotePredicateExitError>;

  PredicateChecker(SemanticState &owner, SemanticParts &all);

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

  static bool looksLikeTest(const Expr *expr);
  static ResultPredicateArg predicateArg(const FunctionDecl *fn, const TypeRef &ref);
  ResultNotAsserting notAsserting(const FunctionDecl *fn, const TypeRef &ref);
  static const VarSymbol *argumentVariable(const Expr *expr);
  static std::optional<Linear> argumentTerm(const Expr *expr);
  static int paramPosition(const FunctionDecl *fn, const VarSymbol *var);
  static void projectSide(const FunctionDecl *fn, const Flow &flow, PredicateSide &side);
  static bool isParamTerm(const FunctionDecl *fn, const Term &t);
  static void projectFacts(const FunctionDecl *fn, const Flow &flow, PredicateSide &side);

  struct PredicateCall {
    const CallExpr *call = nullptr;
    const MemberExpr *member = nullptr;
    const Predicate *summary = nullptr;
  };

  static const Expr *predicateArgument(const PredicateCall &applied, int i);
  const VarSymbol *predicateVariable(const PredicateCall &applied, int i) const;
  void narrowParamTo(Flow &flow, const VarSymbol *var, const Type *type) const;
  void applySide(const PredicateCall &applied, const PredicateSide &side, Flow &flow) const;
  const Predicate *predicateFromCopy(const FuncSymbol *fn) const;
  static int summaryParam(const Predicate *summary, const VarSymbol *var);
  std::optional<Linear> factTerm(const PredicateCall &applied, const Term &t) const;

  SemanticState &state;
  FlowChecker *flowChecker = nullptr;
  LiteralTypes *literalTypes = nullptr;
  TypeResolver *typeResolver = nullptr;
  ReturnInferrer *returnInferrer = nullptr;
  FunctionChecker *functionChecker = nullptr;
};

}
