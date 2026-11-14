#pragma once

#include "compiler/program/ast/ast.h"
#include "compiler/program/types/types.h"
#include "compiler/union/union/union.h"
#include <optional>
#include <utility>
#include <vector>

namespace tz {

struct TypeStorage;

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

enum class NumberSpelling {
  WHOLE,
  FRACTION,
};

class TypesLiterals {
public:
  explicit TypesLiterals(TypeStorage &owner);

  const Type *boolLiteralFor(const Type *expected, bool value);
  const Type *exactMemberFor(const Type *from, const Type *to);
  const Type *unionUnify(const Type *first, const Type *second);
  const Type *unionFallback(const Type *first, const Type *second);
  const Type *stringLiteralFor(const Type *expected, const std::u16string &text);
  const Type *numberMemberOf(const Type *expected, NumberSpelling spelling) const;
  const Type *numberLiteralFor(const Type *expected, double value, NumberSpelling spelling);


  std::optional<std::pair<int64_t, int64_t>> intRange(const Type *type) const;
  std::vector<std::pair<int64_t, int64_t>> intIntervals(const Type *type) const;

private:
  void joinBranches(const JoinBranches &join, std::vector<const Type *> &answers) const;
  void enterExpectation(const EnterExpectation &enter, std::vector<ExpectationStep> &steps,
                        std::vector<const Type *> &answers);


  bool isNumberLike(const Type *type);
  bool isLiteralSet(const Type *type) const;
  bool hasPatternMember(const Type *expected) const;
  bool isIntLiteralExpected(const Type *expected, double value) const;


  const Type *literalPart(const Type *expected, UnionSlot slot) const;
  const Type *literalExpectation(const Type *expected, unsigned depth = 0);


  llvm::ArrayRef<const Type *> membersOf(const Type *&type) const;
  std::vector<const Type *> expectedBranches(const EnterExpectation &enter);

  TypeStorage &types;
};

}
