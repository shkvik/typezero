#pragma once

#include "compiler/semantic/semantic_state/semantic_state.h"
#include "compiler/union/union/union.h"

namespace tz {

class SemanticParts;
class Scopes;
class TypeDeclarer;
class TypeRelations;
class ConditionalTypes;

enum class ExpectationStepKind { ENTER, JOIN };

struct EnterExpectation {
  static constexpr ExpectationStepKind kind = ExpectationStepKind::ENTER;
  const Type *type;
  unsigned depth;
};

struct JoinBranches {
  static constexpr ExpectationStepKind kind = ExpectationStepKind::JOIN;
  size_t count;
};

using ExpectationStep = Union<EnterExpectation, JoinBranches>;

struct TextLookup {
  std::optional<std::vector<std::string>> texts;
  const ConditionalExpr *choice;
};

class LiteralTypes {
public:
  LiteralTypes(SemanticState &owner, SemanticParts &all);

  static const Type *exactMemberFor(const Type *from, const Type *to);
  static const Type *unionFallback(const Type *a, const Type *b);
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
  static std::optional<std::vector<std::string>> typeTexts(const Type *type);
  static void joinBranches(const JoinBranches &join, std::vector<const Type *> &answers);
  void enterExpectation(const EnterExpectation &enter, std::vector<ExpectationStep> &steps,
                        std::vector<const Type *> &answers);
  TextLookup lookupTexts(const Expr *arg);
  static bool choiceTexts(const Expr *e, std::vector<std::u16string> &texts);

  SemanticState &state;
  Scopes *scopes = nullptr;
  TypeDeclarer *typeDeclarer = nullptr;
  TypeRelations *typeRelations = nullptr;
  ConditionalTypes *conditionalTypes = nullptr;
};

}
