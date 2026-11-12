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

  const Type *apparentType(const Type *type) const;
  Expr *apparent(Expr *expr);
  FuncSymbol *genericNamed(const NameExpr *name) const;
  const Type *literalShape(const Expr *expr) const;
  void noteForced(const Expr *expr, const Type *to);
  Expr *forceConversion(Expr *expr, const Type *to);
  std::optional<bool> constantCondition(const Expr *cond) const;
  std::optional<bool> instanceConstant(const Expr *cond) const;
  const Type *narrowedType(const VarSymbol *variable) const;
  const Type *numberParam(const Type *param, const Type *only = nullptr) const;

private:
  NodeChildren<const Expr *> conditionParts(const Expr *cond) const;
  bool isBoolPass(const CastExpr *conversion) const;
  std::optional<bool> nodeCondition(const Expr *cond,
                                    std::vector<std::optional<bool>> &answers) const;
  std::optional<bool> isJsonCondition(const CallExpr *call) const;
  std::optional<bool> logicalCondition(const BinaryExpr *binary,
                                       std::vector<std::optional<bool>> &answers) const;
  std::optional<bool> undefinedCondition(const BinaryExpr *binary) const;
  std::optional<bool> typeOfCondition(const BinaryExpr *binary) const;
  std::optional<bool> typeOfAnswer(const BinaryExpr *binary, const Type *type,
                                   const StringLitExpr *text) const;
  NodeChildren<const Expr *> shapeParts(const Expr *expr) const;
  bool hasGaps(const ArrayLitExpr *literal) const;
  const Type *nodeShape(const Expr *expr, std::vector<const Type *> &answers) const;
  const Type *arrayShape(const ArrayLitExpr *literal, std::vector<const Type *> &answers) const;
  const Type *objectShape(const ObjectLitExpr *literal, std::vector<const Type *> &answers) const;
  bool isBoundedParam(const Type *type) const;
  bool hasBoundedMember(const Type *type) const;
  bool hasOneLiteralBase(const Type *type) const;
  const Type *apparentUnion(const Type *type) const;
  const Type *commonNumericAlternative(const Type *param) const;
  bool isTypeOfTest(const Expr *side, const Expr *other) const;
  bool hasFieldNamed(llvm::ArrayRef<Field> fields, llvm::StringRef name) const;
  std::optional<bool> typeOfSide(const BinaryExpr *binary, const Expr *side,
                                 const Expr *other) const;
  std::optional<bool> undefinedSide(const BinaryExpr *binary, const Expr *side,
                                    const Expr *other) const;

  SemanticState &state;
  Scopes *scopes = nullptr;
  ConcatChecker *concatChecker = nullptr;
  TypeRelations *typeRelations = nullptr;
};

}
