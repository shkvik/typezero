#pragma once

#include "compiler/result/result/result.h"
#include "compiler/semantic/flow/predicate_checker/predicate_checker_errors.h"
#include "compiler/semantic/semantic_state/semantic_state.h"

namespace tz {

class FieldFacts;
class SemanticParts;
class FlowChecker;
class LiteralTypes;
class TypeResolver;
class ReturnInferrer;
class UnionNarrowing;
class FunctionChecker;

class PredicateChecker {
public:
  using ResultExitNarrowed = Result<const Flow *, PredicateCheckerNotePredicateExitError>;

  PredicateChecker(SemanticState &owner, SemanticParts &all);

  const Type *resolveReturnType(FunctionDecl *functionDecl, FuncSymbol *function);
  bool isLookingBoolean(const Expr *expr) const;
  void beginPredicate(FunctionDecl *functionDecl);
  ResultExitNarrowed notePredicateExit(const Flow &whenTrue, const Flow &whenFalse);
  void endPredicate(FunctionDecl *functionDecl);
  const Predicate *predicateOf(const FuncSymbol *function) const;
  void applyPredicate(const CallExpr *call, Flow &whenTrue, Flow &whenFalse) const;
  const Type *filteredType(const Expr *callback, const Type *element) const;

private:
  using ResultNotAsserting = Result<const TypeRef *, PredicateCheckerNotAssertingError>;
  using ResultPredicateArg = Result<int, PredicateCheckerPredicateArgError>;

  struct PredicateCall {
    const CallExpr *call = nullptr;
    const MemberExpr *member = nullptr;
    const Predicate *summary = nullptr;
  };

  const VarSymbol *argumentVariable(const Expr *expr) const;
  const Expr *withoutImplicitCasts(const Expr *expr) const;
  std::optional<Linear> argumentTerm(const Expr *expr) const;
  ResultNotAsserting notAsserting(const FunctionDecl *functionDecl, const TypeRef &ref);
  const Type *predicateType(const TypeRef &ref) const;
  std::string callText(const FunctionDecl *functionDecl) const;
  std::string receiverText(const FunctionDecl *functionDecl) const;
  std::string shownPredicate(const TypeRef &ref, const Type *type) const;
  ResultPredicateArg predicateArg(const FunctionDecl *functionDecl, const TypeRef &ref) const;
  std::optional<int> lastParamNamed(const FunctionDecl *functionDecl, llvm::StringRef name) const;
  void reportAsserting(const FunctionDecl *functionDecl, const TypeRef &ref);
  bool isLookingLikeTest(const Expr *expr) const;
  bool isTestOperator(BinaryOp op) const;
  const VarSymbol *predicatedVariable(const FunctionDecl *functionDecl, int param) const;
  std::vector<const VarSymbol *> paramSymbols(const FunctionDecl *functionDecl) const;
  int paramPosition(const FunctionDecl *functionDecl, const VarSymbol *variable) const;
  PredicateSide projectedSide(const FunctionDecl *functionDecl, const Flow &flow) const;
  std::vector<std::pair<int, const Type *>> projectedNarrowed(const FunctionDecl *functionDecl,
                                                              const Flow &flow) const;
  std::vector<int> projectedDefined(const FunctionDecl *functionDecl, const Flow &flow) const;
  std::vector<std::pair<int, llvm::SmallVector<llvm::StringRef, 2>>>
  projectedFields(const FunctionDecl *functionDecl, const Flow &flow) const;
  std::vector<PredicateSide::NarrowedField>
  projectedNarrowedFields(const FunctionDecl *functionDecl, const Flow &flow) const;
  bool isParamTerm(const FunctionDecl *functionDecl, const Term &term) const;
  std::vector<Facts::Fact> projectedFacts(const FunctionDecl *functionDecl, const Flow &flow) const;
  bool isSummarizable(const FuncSymbol *function, const FunctionDecl *functionDecl) const;
  const Predicate *predicateFromCopy(const FuncSymbol *function) const;
  const Expr *predicateArgument(const PredicateCall &applied, int index) const;
  const VarSymbol *predicateVariable(const PredicateCall &applied, int index) const;
  void narrowParamTo(Flow &flow, const VarSymbol *variable, const Type *type) const;
  std::vector<const Type *> convertibleMembers(const Type *value, const Type *type) const;
  void applySide(const PredicateCall &applied, const PredicateSide &side, Flow &flow) const;
  void applyNarrowed(const PredicateCall &applied, const PredicateSide &side, Flow &flow) const;
  void applyDefined(const PredicateCall &applied, const PredicateSide &side, Flow &flow) const;
  void applyFields(const PredicateCall &applied, const PredicateSide &side, Flow &flow) const;
  void applyNarrowedFields(const PredicateCall &applied, const PredicateSide &side,
                           Flow &flow) const;
  bool isPathEntry(const FieldPath &path, const std::pair<FieldPath, const Type *> &entry) const;
  void narrowFieldTo(Flow &flow, const FieldPath &path, const Type *type) const;
  void applyFacts(const PredicateCall &applied, const PredicateSide &side, Flow &flow) const;
  int summaryParam(const Predicate *summary, const VarSymbol *variable) const;
  std::optional<Linear> factTerm(const PredicateCall &applied, const Term &term) const;
  std::optional<Linear> lengthTerm(const PredicateCall &applied, int index) const;
  const FuncSymbol *callbackFunction(const Expr *callback) const;
  const FuncSymbol *namedFunction(const NameExpr *name) const;
  const Type *keptByPredicate(const FuncSymbol *function, const Type *element) const;
  const Type *keptBySide(const PredicateSide &side, const Type *element) const;
  const Type *firstParamNarrowed(const PredicateSide &side) const;

  SemanticState &state;
  FieldFacts *fieldFacts = nullptr;
  FlowChecker *flowChecker = nullptr;
  LiteralTypes *literalTypes = nullptr;
  TypeResolver *typeResolver = nullptr;
  ReturnInferrer *returnInferrer = nullptr;
  UnionNarrowing *unionNarrowing = nullptr;
  FunctionChecker *functionChecker = nullptr;
};

}
