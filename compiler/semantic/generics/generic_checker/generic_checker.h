#pragma once

#include "compiler/semantic/flow/children_first/children_first.h"
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
  static NodeChildren<const Expr *> conditionParts(const Expr *cond);
  static bool isBoolPass(const CastExpr *conv);
  static std::optional<bool> nodeCondition(const Expr *cond,
                                           std::vector<std::optional<bool>> &answers);
  static std::optional<bool> isJsonCondition(const CallExpr *call);
  static std::optional<bool> logicalCondition(const BinaryExpr *bin,
                                              std::vector<std::optional<bool>> &answers);
  static std::optional<bool> undefinedCondition(const BinaryExpr *bin);
  static std::optional<bool> typeOfCondition(const BinaryExpr *bin);
  static std::optional<bool> typeOfAnswer(const BinaryExpr *bin, const Type *type,
                                          const StringLitExpr *text);
  static NodeChildren<const Expr *> shapeParts(const Expr *expr);
  static bool hasGaps(const ArrayLitExpr *lit);
  static const Type *nodeShape(const Expr *expr, std::vector<const Type *> &answers);
  static const Type *arrayShape(const ArrayLitExpr *lit, std::vector<const Type *> &answers);
  static const Type *objectShape(const ObjectLitExpr *lit, std::vector<const Type *> &answers);
  SemanticState &state;
  Scopes *scopes = nullptr;
  ConcatChecker *concatChecker = nullptr;
  TypeRelations *typeRelations = nullptr;
};

}
