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

enum class NumberSpelling {
  WHOLE,
  FRACTION,
};

class LiteralTypes {
public:
  LiteralTypes(SemanticState &owner, SemanticParts &all);

  static const Type *unionUnify(const Type *first, const Type *second);
  static const Type *exactMemberFor(const Type *from, const Type *to);
  static const Type *unionFallback(const Type *first, const Type *second);

  const Type *enumTag(const Expr *expr) const;
  Expr *constText(Expr *checked, const Type *expected);
  const Type *boolLiteralFor(const Type *expected, bool value);
  const Type *constTextType(const Expr *checked, const Type *expected);
  std::optional<std::vector<std::string>> knownTexts(const Expr *arg);
  const Type *typeIn(const Flow &flow, const VarSymbol *variable) const;
  const Type *literalExpectation(const Type *expected, unsigned depth = 0);
  std::optional<std::pair<int64_t, int64_t>> intRange(const Type *type) const;
  std::vector<std::pair<int64_t, int64_t>> intIntervals(const Type *type) const;
  const Type *numberMemberOf(const Type *expected, NumberSpelling spelling) const;
  const Type *stringLiteralFor(const Type *expected, const std::u16string &text);
  const Type *literalMember(const ObjectLitExpr *literal, const Type *target) const;
  const Type *numberLiteralFor(const Type *expected, double value, NumberSpelling spelling);

private:
  static bool isNumberLike(const Type *type);

  bool isLiteralSet(const Type *type) const;
  TextLookup lookupTexts(const Expr *arg);
  bool hasSpread(const ObjectLitExpr *literal) const;
  bool hasPatternMember(const Type *expected) const;
  bool isPlainObjectMember(const Type *member) const;
  const Expr *writtenValue(const Expr *bare) const;
  const Type *knownLiteralSet(const Expr *value) const;
  const VarSymbol *namedVariable(const Expr *expr) const;
  bool isTagConvertible(const Type *tag, const Type *fieldType) const;
  const Type *literalPart(const Type *expected, UnionSlot slot) const;
  bool isIntLiteralExpected(const Type *expected, double value) const;
  std::optional<std::u16string> writtenText(const Expr *expr) const;
  llvm::ArrayRef<const Type *> membersOf(const Type *&type) const;
  std::vector<const Type *> expectedBranches(const EnterExpectation &enter);
  std::optional<std::vector<std::string>> typeTexts(const Type *type) const;
  std::optional<std::vector<std::string>> typedTexts(const Expr *arg) const;
  bool hasProperty(const ObjectLitExpr *literal, llvm::StringRef name) const;
  const Type *textLiteralFor(const Type *expected, const std::u16string &text);
  bool hasFittingNames(const ObjectLitExpr *literal, const Type *member) const;
  std::optional<std::vector<std::string>> narrowedTexts(const Expr *bare) const;
  std::optional<std::vector<std::u16string>> choiceTexts(const Expr *expr) const;
  void joinBranches(const JoinBranches &join, std::vector<const Type *> &answers) const;
  std::optional<int> memberScore(const ObjectLitExpr *literal, const Type *member) const;
  std::optional<int> candidateScore(const ObjectLitExpr *literal, const Type *member) const;
  const Type *propertyTag(const ObjectLitExpr::Property &property, const Field *field) const;
  std::optional<int> propertyScore(const ObjectLitExpr::Property &property,
                                   const Type *member) const;
  void enterExpectation(const EnterExpectation &enter, std::vector<ExpectationStep> &steps,
                        std::vector<const Type *> &answers);

  SemanticState &state;
  Scopes *scopes = nullptr;
  TypeDeclarer *typeDeclarer = nullptr;
  TypeRelations *typeRelations = nullptr;
  ConditionalTypes *conditionalTypes = nullptr;
};

}
