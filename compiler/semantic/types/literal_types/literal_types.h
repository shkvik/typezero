#pragma once

#include "compiler/semantic/semantic_state/semantic_state.h"

namespace tz {

class SemanticParts;
class Scopes;
class TypeDeclarer;
class TypeRelations;
class ConditionalTypes;

class LiteralTypes {
public:
  LiteralTypes(SemanticState &owner, SemanticParts &all);

  static const Type *unionMemberFor(const Type *from, const Type *to);
  static const Type *unionUnify(const Type *a, const Type *b);
  const Type *stringLiteralFor(const Type *expected, const std::u16string &text);
  const Type *boolLiteralFor(const Type *expected, bool value);
  const Type *numberLiteralFor(const Type *expected, double value, bool fraction);
  static const Type *numberMemberOf(const Type *expected, bool fraction);
  static std::optional<std::pair<int64_t, int64_t>> intRange(const Type *type);
  static std::vector<std::pair<int64_t, int64_t>> intIntervals(const Type *type);
  const Type *literalMember(const ObjectLitExpr *lit, const Type *target) const;
  const Type *enumTag(const Expr *e) const;
  const Type *typeIn(const Flow &flow, const VarSymbol *var) const;
  std::optional<std::vector<std::string>> knownTexts(const Expr *arg);
  const Type *constTextType(const Expr *checked, const Type *expected);
  Expr *constText(Expr *checked, const Type *expected);
  const Type *literalExpectation(const Type *expected, unsigned depth = 0);

private:
  static llvm::ArrayRef<const Type *> membersOf(const Type *&type);
  static bool isLiteralSet(const Type *type);
  static const Type *literalPart(const Type *expected, UnionSlot slot);
  static std::optional<std::u16string> writtenText(const Expr *e);
  static bool choiceTexts(const Expr *e, std::vector<std::u16string> &texts);

  SemanticState &state;
  Scopes *scopes = nullptr;
  TypeDeclarer *typeDeclarer = nullptr;
  TypeRelations *typeRelations = nullptr;
  ConditionalTypes *conditionalTypes = nullptr;
};

}
