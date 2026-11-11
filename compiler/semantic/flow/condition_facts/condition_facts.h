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
  static const VarSymbol *variableOf(const Expr *expr);
  static const NameExpr *optionalVariable(const Expr *expr);
  static std::optional<FieldPath> definedPath(const Expr *expr, Flow &flow);
  static const Expr *pathInner(const Expr *expr);
  static std::optional<FieldPath> pathRoot(const Expr *expr);
  static void extendPath(const Expr *link, FieldPath &path, Flow &flow);
  static void addLess(Facts &facts, const Linear &x, const Linear &y, bool orEqual);

  bool chainDefined(const Expr *e, Flow &flow) const;
  std::optional<FieldPath> optionalField(const Expr *expr) const;
  void addBinaryFacts(const BinaryExpr *bin, Flow &whenTrue, Flow &whenFalse) const;
  static const Expr *withoutPlainCasts(const Expr *expr);
  void addChainFacts(const BinaryExpr *bin, Flow &whenTrue, Flow &whenFalse) const;
  void addInstanceOfFacts(const BinaryExpr *bin, Flow &whenTrue) const;
  void addUndefinedFacts(const BinaryExpr *bin, Flow &whenTrue, Flow &whenFalse) const;
  static void addConvertedFacts(const BinaryExpr *bin, Flow &whenTrue, Flow &whenFalse);
  void addTypeOfFacts(const BinaryExpr *bin, Flow &whenTrue, Flow &whenFalse) const;
  void addParamTypeOfFacts(const VarSymbol *var, const std::u16string &text, Flow &holds,
                           Flow &differs) const;
  const Type *paramTypeOfNarrowed(const Type *param, const std::u16string &text) const;
  static bool isStringType(const Type *t);
  static bool isBoolType(const Type *t);
  static bool admits(const Type *param, bool (*test)(const Type *));
  const Type *numberNamed(const Type *param, llvm::StringRef name) const;
  const Type *otherAlternatives(const Type *param, const std::u16string &text) const;
  void addLinearFacts(const BinaryExpr *bin, Flow &whenTrue, Flow &whenFalse) const;
  static bool constantHolds(BinaryOp op, int64_t a, int64_t b);
  void addDifferFacts(BinaryOp op, const Linear &l, const Linear &r, Facts &t, Facts &f) const;
  static void addOrderFacts(BinaryOp op, const Linear &l, const Linear &r, Facts &t, Facts &f);

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
