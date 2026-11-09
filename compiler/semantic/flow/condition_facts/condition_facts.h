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
  static void addLess(Facts &facts, const Linear &x, const Linear &y, bool orEqual);

  bool chainDefined(const Expr *e, Flow &flow) const;

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
