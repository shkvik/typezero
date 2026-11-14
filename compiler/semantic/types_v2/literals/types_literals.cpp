#include "compiler/semantic/types_v2/literals/types_literals.h"
#include "compiler/program/to_utf8/to_utf8.h"
#include "compiler/semantic/flow/children_first/children_first.h"
#include "compiler/semantic/semantic_state/v2/types/type_storage.h"
#include "llvm/Support/ConvertUTF.h"

namespace tz {

namespace {

constexpr unsigned kMaxExpectationDepth = 8;

}

TypesLiterals::TypesLiterals(TypeStorage &owner)
    : types(owner) {}

llvm::ArrayRef<const Type *> TypesLiterals::membersOf(const Type *&type) const {
  return type->isUnion() ? type->members() : llvm::ArrayRef<const Type *>(type);
}

bool TypesLiterals::isLiteralSet(const Type *type) const {
  if (type->isLiteral()) {
    return true;
  }
  if (!type->isUnion()) {
    return false;
  }
  for (const Type *member : type->members()) {
    if (!member->isLiteral()) {
      return false;
    }
  }

  return true;
}

const Type *TypesLiterals::literalPart(const Type *expected, UnionSlot slot) const {
  if (expected == nullptr) {
    return nullptr;
  }
  const Type *part = expected->nonOptional()->slotPart(slot);
  if (part == nullptr || !this->isLiteralSet(part)) {
    return nullptr;
  }

  return part;
}

const Type *TypesLiterals::exactMemberFor(const Type *from, const Type *to) {
  for (const Type *member : to->members()) {
    if (member == from) {
      return member;
    }
  }
  if (!from->isLiteral()) {
    return nullptr;
  }
  for (const Type *member : to->members()) {
    if (member == from->literalBase()) {
      return member;
    }
  }

  return nullptr;
}

const Type *TypesLiterals::unionUnify(const Type *first, const Type *second) {
  return this->types.relations.unifyAs(UnifyForm::UNION, first, second);
}

const Type *TypesLiterals::unionFallback(const Type *first, const Type *second) {
  if (first->isError() || second->isError()) {
    return Type::error();
  }
  if (first->isVoid() || second->isVoid()) {
    return nullptr;
  }
  if (this->isNumberLike(first) && this->isNumberLike(second)) {
    return nullptr;
  }
  const auto *joined = Type::unionOf({first, second});
  if (joined->isError() || !UnionSlots::unionProblem(joined).empty()) {
    return nullptr;
  }

  return joined;
}

bool TypesLiterals::isNumberLike(const Type *type) {
  const Type *plain = type->nonOptional();

  return plain->isNumeric() || plain->isDecimal();
}

const Type *TypesLiterals::literalExpectation(const Type *expected, unsigned depth) {
  auto first = EnterExpectation{.type = expected, .depth = depth};
  auto steps = std::vector<ExpectationStep>{first};
  auto answers = std::vector<const Type *>();
  while (!steps.empty()) {
    ExpectationStep step = steps.back();
    steps.pop_back();
    switch (step.kind()) {
      case ExpectationStepKind::ENTER:
        this->enterExpectation(*step.as<EnterExpectation>(), steps, answers);
        break;
      case ExpectationStepKind::JOIN:
        this->joinBranches(*step.as<JoinBranches>(), answers);
        break;
    }
  }

  return answers.back();
}

void TypesLiterals::enterExpectation(const EnterExpectation &enter,
                                     std::vector<ExpectationStep> &steps,
                                     std::vector<const Type *> &answers) {
  auto branches = this->expectedBranches(enter);
  if (branches.empty()) {
    answers.push_back(enter.type);
    return;
  }
  steps.push_back(JoinBranches{.count = branches.size()});
  for (const Type *branch : llvm::reverse(branches)) {
    steps.push_back(EnterExpectation{.type = branch, .depth = enter.depth + 1});
  }
}

std::vector<const Type *> TypesLiterals::expectedBranches(const EnterExpectation &enter) {
  const Type *expected = enter.type;
  bool isConditional = expected != nullptr && enter.depth <= kMaxExpectationDepth &&
                       expected->nonOptional()->typeOp() == TypeOp::Conditional;
  if (!isConditional) {
    return {};
  }

  return this->types.context.findConditionalBranches(expected->nonOptional());
}

void TypesLiterals::joinBranches(const JoinBranches &join,
                                 std::vector<const Type *> &answers) const {
  auto members = ChildrenFirst::takeAnswers(answers, join.count);
  const auto *joined = Type::unionOf(members);
  answers.push_back(joined);
}

const Type *TypesLiterals::stringLiteralFor(const Type *expected, const std::u16string &text) {
  const Type *plain = this->literalExpectation(expected);
  bool isAtomExpected = this->literalPart(plain, UnionSlot::Atom) != nullptr;
  if (isAtomExpected || this->hasPatternMember(plain)) {
    return Type::stringLiteral(text);
  }

  return nullptr;
}

bool TypesLiterals::hasPatternMember(const Type *expected) const {
  if (expected == nullptr) {
    return false;
  }
  const Type *strings = expected->nonOptional()->slotPart(UnionSlot::String);
  if (strings == nullptr) {
    return false;
  }
  for (const Type *member : this->membersOf(strings)) {
    if (member->isPattern()) {
      return true;
    }
  }

  return false;
}

const Type *TypesLiterals::boolLiteralFor(const Type *expected, bool value) {
  const Type *plain = this->literalExpectation(expected);
  if (this->literalPart(plain, UnionSlot::Bool) == nullptr) {
    return nullptr;
  }

  return Type::boolLiteral(value);
}

const Type *TypesLiterals::numberLiteralFor(const Type *expected, double value,
                                            NumberSpelling spelling) {
  const Type *plain = this->literalExpectation(expected);
  bool hasDoubles = this->literalPart(plain, UnionSlot::Double) != nullptr;
  bool isWhole = spelling == NumberSpelling::WHOLE;
  if (isWhole && this->isIntLiteralExpected(plain, value)) {
    return Type::numberLiteral(value, Type::intTy());
  }
  if (hasDoubles) {
    return Type::numberLiteral(value, Type::doubleTy());
  }

  return nullptr;
}

bool TypesLiterals::isIntLiteralExpected(const Type *expected, double value) const {
  const Type *ints = this->literalPart(expected, UnionSlot::Int);
  if (ints == nullptr) {
    return false;
  }
  const auto *literal = Type::numberLiteral(value, Type::intTy());
  bool isListed = llvm::is_contained(this->membersOf(ints), literal);
  bool hasDoubles = this->literalPart(expected, UnionSlot::Double) != nullptr;

  return isListed || !hasDoubles;
}

std::optional<std::pair<int64_t, int64_t>> TypesLiterals::intRange(const Type *type) const {
  if (!type->isUnion()) {
    return std::nullopt;
  }
  int64_t low = INT64_MAX;
  int64_t high = INT64_MIN;
  for (const Type *member : type->members()) {
    if (!member->isLiteral() || !member->literalBase()->isInt()) {
      return std::nullopt;
    }
    int64_t value = int64_t(member->literalNumber());
    low = std::min(low, value);
    high = std::max(high, value);
  }
  bool isDense = high - low + 1 == int64_t(type->members().size());
  if (!isDense) {
    return std::nullopt;
  }

  return std::pair(low, high);
}

std::vector<std::pair<int64_t, int64_t>> TypesLiterals::intIntervals(const Type *type) const {
  auto intervals = std::vector<std::pair<int64_t, int64_t>>();
  if (!type->isUnion()) {
    return intervals;
  }
  auto values = std::vector<int64_t>();
  for (const Type *member : type->members()) {
    if (!member->isLiteral() || !member->literalBase()->isInt()) {
      return intervals;
    }
    values.push_back(int64_t(member->literalNumber()));
  }
  llvm::sort(values);
  for (int64_t value : values) {
    bool isJoining = !intervals.empty() && intervals.back().second + 1 >= value;
    if (isJoining) {
      intervals.back().second = std::max(intervals.back().second, value);
    } else {
      intervals.emplace_back(value, value);
    }
  }

  return intervals;
}

const Type *TypesLiterals::numberMemberOf(const Type *expected, NumberSpelling spelling) const {
  if (expected == nullptr) {
    return nullptr;
  }
  const Type *type = expected->nonOptional();
  if (!type->isUnion()) {
    return nullptr;
  }
  for (const Type *candidate : type->members()) {
    bool isNumber = candidate->isInteger() || candidate->isFloat() || candidate->isDecimal();
    bool isFitting = spelling == NumberSpelling::WHOLE || !candidate->isInteger();
    if (isNumber && isFitting) {
      return candidate;
    }
  }

  return nullptr;
}

}
