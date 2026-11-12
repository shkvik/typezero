#pragma once

#include "compiler/semantic/semantic_state/semantic_state.h"

namespace tz {

class SemanticParts;
class FieldFacts;
class IndexProofs;
class JsonChecker;
class ResultChecker;
class GenericChecker;
class UnionNarrowing;
class UnknownChecker;
class PredicateChecker;

class ConditionFacts {
public:
  ConditionFacts(SemanticState &owner, SemanticParts &all);

  void addConditionFacts(const Expr *cond, Flow &whenTrue, Flow &whenFalse) const;

private:
  const VarSymbol *variableOf(const Expr *expr) const;
  const NameExpr *optionalVariable(const Expr *expr) const;
  void addLess(Facts &facts, const Linear &smaller, const Linear &larger) const;
  void addAtMost(Facts &facts, const Linear &smaller, const Linear &larger) const;
  bool isOptionalChain(const Expr *expr) const;
  void defineChain(const Expr *expr, Flow &flow) const;
  std::optional<FieldPath> definedPath(const Expr *expr, Flow &flow) const;
  const Expr *pathInner(const Expr *expr) const;
  std::optional<FieldPath> pathRoot(const Expr *expr) const;
  void extendPath(const Expr *link, FieldPath &path, Flow &flow) const;
  void addOwnFacts(const Expr *cond, Flow &whenTrue, Flow &whenFalse) const;
  std::optional<FieldPath> optionalField(const Expr *expr) const;
  void defineOptional(const Expr *expr, Flow &flow) const;
  void addBinaryFacts(const BinaryExpr *binary, Flow &whenTrue, Flow &whenFalse) const;
  const Expr *withoutPlainCasts(const Expr *expr) const;
  bool isDefinedValue(const Type *type) const;
  void addChainFacts(const BinaryExpr *binary, Flow &whenTrue, Flow &whenFalse) const;
  void addSideChainFacts(const Expr *side, const Expr *other, Flow &whenEqual,
                         Flow &whenUnequal) const;
  void addInstanceOfFacts(const BinaryExpr *binary, Flow &whenTrue) const;
  const Type *instanceType(const Type *type) const;
  bool isSubclassOf(const Type *classType, const Type *own) const;
  void addUndefinedFacts(const BinaryExpr *binary, Flow &whenTrue, Flow &whenFalse) const;
  void addConvertedFacts(const BinaryExpr *binary, Flow &whenTrue, Flow &whenFalse) const;
  void addSideConvertedFacts(const Expr *side, const Expr *other, Flow &whenEqual) const;
  void addTypeOfFacts(const BinaryExpr *binary, Flow &whenTrue, Flow &whenFalse) const;
  void addSideTypeOfFacts(const Expr *side, const Expr *other, Flow &whenEqual,
                          Flow &whenUnequal) const;
  void addParamTypeOfFacts(const VarSymbol *variable, const std::u16string &text, Flow &holds,
                           Flow &differs) const;
  const Type *paramTypeOfNarrowed(const Type *param, const std::u16string &text) const;
  bool canBe(const Type *param, bool (Type::*test)() const) const;
  const Type *numberNamed(const Type *param, llvm::StringRef name) const;
  const Type *otherAlternatives(const Type *param, const std::u16string &text) const;
  llvm::SmallVector<const Type *, 4> alternativesNotNamed(const Type *param,
                                                          llvm::StringRef name) const;
  bool areAllNamed(llvm::ArrayRef<const Type *> types, llvm::StringRef name) const;
  void addLinearFacts(const BinaryExpr *binary, Flow &whenTrue, Flow &whenFalse) const;
  bool isConstantHolding(BinaryOp op, int64_t left, int64_t right) const;
  void addDifferFacts(BinaryOp op, const Linear &left, const Linear &right, Facts &trueFacts,
                      Facts &falseFacts) const;
  void addOrderFacts(BinaryOp op, const Linear &left, const Linear &right, Facts &trueFacts,
                     Facts &falseFacts) const;

  SemanticState &state;
  FieldFacts *fieldFacts = nullptr;
  IndexProofs *indexProofs = nullptr;
  JsonChecker *jsonChecker = nullptr;
  ResultChecker *resultChecker = nullptr;
  GenericChecker *genericChecker = nullptr;
  UnionNarrowing *unionNarrowing = nullptr;
  UnknownChecker *unknownChecker = nullptr;
  PredicateChecker *predicateChecker = nullptr;
};

}
