#pragma once

#include "compiler/program/ast/ast.h"
#include "compiler/program/types/types.h"
#include "compiler/semantic/semantic_state/flow_state/flow_state_types.h"

namespace tz {

class SemanticState;

class FlowFunctionFacts {
public:
  explicit FlowFunctionFacts(SemanticState &owner);

  void applyPredicate(const CallExpr *call, Flow &whenTrue, Flow &whenFalse) const;


  bool isLookingBoolean(const Expr *expr) const;
  bool isPredicateNeeded(const FuncSymbol *function) const;


  const FuncSymbol *findPredicateNeed(const Expr *cond) const;
  const FuncSymbol *findFilterNeed(const Expr *callback) const;


  const Type *filteredType(const Expr *callback, const Type *element) const;

private:
  struct PredicateCall {
    const CallExpr *call = nullptr;
    const MemberExpr *member = nullptr;
    const Predicate *summary = nullptr;
  };

  void narrowFieldTo(Flow &flow, const FieldPath &path, const Type *type) const;
  void narrowParamTo(Flow &flow, const VarSymbol *variable, const Type *type) const;
  void applySide(const PredicateCall &applied, const PredicateSide &side, Flow &flow) const;
  void applyFacts(const PredicateCall &applied, const PredicateSide &side, Flow &flow) const;
  void applyFields(const PredicateCall &applied, const PredicateSide &side, Flow &flow) const;
  void applyDefined(const PredicateCall &applied, const PredicateSide &side, Flow &flow) const;
  void applyNarrowed(const PredicateCall &applied, const PredicateSide &side, Flow &flow) const;
  void applyNarrowedFields(const PredicateCall &applied, const PredicateSide &side,
                           Flow &flow) const;


  bool isTestOperator(BinaryOp op) const;
  bool isLookingLikeTest(const Expr *expr) const;
  bool isSummarizable(const FuncSymbol *function, const FunctionDecl *functionDecl) const;
  bool isPathEntry(const FieldPath &path, const std::pair<FieldPath, const Type *> &entry) const;


  const Expr *withoutImplicitCasts(const Expr *expr) const;
  const Expr *predicateArgument(const PredicateCall &applied, int index) const;


  const Type *firstParamNarrowed(const PredicateSide &side) const;
  const Type *keptBySide(const PredicateSide &side, const Type *element) const;
  const Type *keptByPredicate(const FuncSymbol *function, const Type *element) const;


  const VarSymbol *argumentVariable(const Expr *expr) const;
  const VarSymbol *predicateVariable(const PredicateCall &applied, int index) const;


  const FuncSymbol *namedFunction(const NameExpr *name) const;
  const FuncSymbol *callbackFunction(const Expr *callback) const;


  std::optional<Linear> argumentTerm(const Expr *expr) const;
  std::optional<Linear> lengthTerm(const PredicateCall &applied, int index) const;
  std::optional<Linear> factTerm(const PredicateCall &applied, const Term &term) const;


  int summaryParam(const Predicate *summary, const VarSymbol *variable) const;
  std::vector<const Type *> convertibleMembers(const Type *value, const Type *type) const;

  SemanticState &state;
};

}
