#pragma once

#include "compiler/semantic/semantic_state/semantic_state.h"

namespace tz {

class SemanticParts;
class Scopes;
class ConcatChecker;
class TypeRelations;

class GenericChecker {
public:
  GenericChecker(SemanticState &owner, SemanticParts &all);

  static const Type *apparentType(const Type *type);
  Expr *apparent(Expr *expr);
  FuncSymbol *genericNamed(const NameExpr *name) const;
  static const Type *literalShape(const Expr *expr);
  void noteForced(const Expr *expr, const Type *to);
  Expr *forceConversion(Expr *expr, const Type *to);
  std::optional<bool> constantCondition(const Expr *cond) const;
  const Type *narrowedType(const VarSymbol *var) const;
  const Type *numberParam(const Type *param, const Type *only = nullptr) const;

private:
  SemanticState &state;
  Scopes *scopes = nullptr;
  ConcatChecker *concatChecker = nullptr;
  TypeRelations *typeRelations = nullptr;
};

}
