#include "compiler/semantic/types/literal_types/literal_types.h"
#include "compiler/program/to_utf8/to_utf8.h"
#include "compiler/semantic/declare/type_declarer/type_declarer.h"
#include "compiler/semantic/flow/children_first/children_first.h"
#include "compiler/semantic/flow/expr_parts/expr_parts.h"
#include "compiler/semantic/names/scopes/scopes.h"
#include "compiler/semantic/semantic_parts/semantic_parts.h"
#include "compiler/semantic/types/conditional_types/conditional_types.h"
#include "compiler/semantic/types/type_relations/type_relations.h"
#include "compiler/semantic/types/union_check_helpers/union_check_helpers.h"
#include "llvm/Support/ConvertUTF.h"

using llvm::dyn_cast;

namespace tz {

namespace {

constexpr unsigned kMaxExpectationDepth = 8;

}

LiteralTypes::LiteralTypes(SemanticState &owner, SemanticParts &all)
    : state(owner) {
  this->scopes = &all.scopes;
  this->typeDeclarer = &all.typeDeclarer;
  this->typeRelations = &all.typeRelations;
  this->conditionalTypes = &all.conditionalTypes;
}

llvm::ArrayRef<const Type *> LiteralTypes::membersOf(const Type *&type) const {
  return type->isUnion() ? type->members() : llvm::ArrayRef<const Type *>(type);
}

bool LiteralTypes::isLiteralSet(const Type *type) const {
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

const Type *LiteralTypes::literalPart(const Type *expected, UnionSlot slot) const {
  if (expected == nullptr) {
    return nullptr;
  }
  const Type *part = expected->nonOptional()->slotPart(slot);
  if (part == nullptr || !this->isLiteralSet(part)) {
    return nullptr;
  }

  return part;
}

std::optional<std::u16string> LiteralTypes::writtenText(const Expr *expr) const {
  auto *literal = dyn_cast<StringLitExpr>(expr);
  auto *templateExpr = dyn_cast<TemplateExpr>(expr);

  if (literal != nullptr) {
    return literal->value;
  }
  if (templateExpr != nullptr && templateExpr->exprs.empty()) {
    return templateExpr->quasis[0];
  }

  return std::nullopt;
}

std::optional<std::vector<std::u16string>> LiteralTypes::choiceTexts(const Expr *expr) const {
  auto texts = std::vector<std::u16string>();
  for (const Expr *branch : ExprParts::choiceBranches(expr)) {
    auto text = this->writtenText(branch);
    if (!text.has_value()) {
      return std::nullopt;
    }
    if (!llvm::is_contained(texts, *text)) {
      texts.push_back(*text);
    }
  }

  return texts;
}

const Type *LiteralTypes::enumTag(const Expr *expr) const {
  auto *member = dyn_cast<MemberExpr>(expr);
  if (member == nullptr) {
    return nullptr;
  }
  auto *owner = dyn_cast<NameExpr>(member->object);
  if (owner == nullptr) {
    return nullptr;
  }
  auto entry = this->scopes->lookup(owner->name);
  EnumDecl *enumDecl = entry.enumDecl();
  if (enumDecl == nullptr) {
    return nullptr;
  }
  this->typeDeclarer->resolveEnum(enumDecl);
  const auto *enumMember = enumDecl->find(member->name);
  if (enumMember == nullptr) {
    return nullptr;
  }
  if (enumMember->isString) {
    return Type::stringLiteral(enumMember->text);
  }

  return Type::numberLiteral(double(enumMember->number), Type::intTy());
}

const Type *LiteralTypes::constTextType(const Expr *checked, const Type *expected) {
  auto *name = dyn_cast<NameExpr>(checked);
  bool isTextConstant = name != nullptr && name->var() != nullptr && name->var()->literalConst &&
                        checked->type->isString();
  if (!isTextConstant) {
    return nullptr;
  }
  const auto *written = this->state.getConstantStore().findConstLiteral(name->var());
  if (written == nullptr) {
    return nullptr;
  }
  auto texts = this->choiceTexts(written);
  if (!texts.has_value()) {
    return nullptr;
  }
  auto literals = std::vector<const Type *>();
  for (const std::u16string &text : *texts) {
    const Type *literal = this->textLiteralFor(expected, text);
    if (literal == nullptr) {
      return nullptr;
    }
    literals.push_back(literal);
  }

  return Type::unionOf(literals);
}

const Type *LiteralTypes::textLiteralFor(const Type *expected, const std::u16string &text) {
  if (expected == nullptr) {
    return Type::stringLiteral(text);
  }

  return this->stringLiteralFor(expected, text);
}

Expr *LiteralTypes::constText(Expr *checked, const Type *expected) {
  if (expected == nullptr) {
    return checked;
  }
  const auto *type = this->constTextType(checked, expected);
  if (type == nullptr) {
    return checked;
  }

  return this->typeRelations->convert(checked, type);
}

const Type *LiteralTypes::exactMemberFor(const Type *from, const Type *to) {
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

const Type *LiteralTypes::unionUnify(const Type *first, const Type *second) {
  return TypeRelations::unifyAs(UnifyForm::UNION, first, second);
}

const Type *LiteralTypes::unionFallback(const Type *first, const Type *second) {
  if (first->isError() || second->isError()) {
    return Type::error();
  }
  if (first->isVoid() || second->isVoid()) {
    return nullptr;
  }
  if (LiteralTypes::isNumberLike(first) && LiteralTypes::isNumberLike(second)) {
    return nullptr;
  }
  const auto *joined = Type::unionOf({first, second});
  if (joined->isError() || !UnionSlots::unionProblem(joined).empty()) {
    return nullptr;
  }

  return joined;
}

bool LiteralTypes::isNumberLike(const Type *type) {
  const Type *plain = type->nonOptional();

  return plain->isNumeric() || plain->isDecimal();
}

const Type *LiteralTypes::literalExpectation(const Type *expected, unsigned depth) {
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

void LiteralTypes::enterExpectation(const EnterExpectation &enter,
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

std::vector<const Type *> LiteralTypes::expectedBranches(const EnterExpectation &enter) {
  const Type *expected = enter.type;
  bool isConditional = expected != nullptr && enter.depth <= kMaxExpectationDepth &&
                       expected->nonOptional()->typeOp() == TypeOp::Conditional;
  if (!isConditional) {
    return {};
  }

  return this->conditionalTypes->conditionalBranches(expected->nonOptional());
}

void LiteralTypes::joinBranches(const JoinBranches &join,
                                std::vector<const Type *> &answers) const {
  auto members = ChildrenFirst::takeAnswers(answers, join.count);
  const auto *joined = Type::unionOf(members);
  answers.push_back(joined);
}

const Type *LiteralTypes::stringLiteralFor(const Type *expected, const std::u16string &text) {
  const Type *plain = this->literalExpectation(expected);
  bool isAtomExpected = this->literalPart(plain, UnionSlot::Atom) != nullptr;
  if (isAtomExpected || this->hasPatternMember(plain)) {
    return Type::stringLiteral(text);
  }

  return nullptr;
}

bool LiteralTypes::hasPatternMember(const Type *expected) const {
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

const Type *LiteralTypes::boolLiteralFor(const Type *expected, bool value) {
  const Type *plain = this->literalExpectation(expected);
  if (this->literalPart(plain, UnionSlot::Bool) == nullptr) {
    return nullptr;
  }

  return Type::boolLiteral(value);
}

const Type *LiteralTypes::numberLiteralFor(const Type *expected, double value,
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

bool LiteralTypes::isIntLiteralExpected(const Type *expected, double value) const {
  const Type *ints = this->literalPart(expected, UnionSlot::Int);
  if (ints == nullptr) {
    return false;
  }
  const auto *literal = Type::numberLiteral(value, Type::intTy());
  bool isListed = llvm::is_contained(this->membersOf(ints), literal);
  bool hasDoubles = this->literalPart(expected, UnionSlot::Double) != nullptr;

  return isListed || !hasDoubles;
}

std::optional<std::pair<int64_t, int64_t>> LiteralTypes::intRange(const Type *type) const {
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

std::vector<std::pair<int64_t, int64_t>> LiteralTypes::intIntervals(const Type *type) const {
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

const Type *LiteralTypes::numberMemberOf(const Type *expected, NumberSpelling spelling) const {
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

const Type *LiteralTypes::literalMember(const ObjectLitExpr *literal, const Type *target) const {
  if (this->hasSpread(literal)) {
    return nullptr;
  }
  const Type *best = nullptr;
  int bestScore = -1;
  bool isAmbiguous = false;
  for (const Type *member : target->members()) {
    auto score = this->candidateScore(literal, member);
    if (score.has_value() && *score > bestScore) {
      best = member;
      bestScore = *score;
      isAmbiguous = false;
    } else if (score.has_value() && *score == bestScore) {
      isAmbiguous = true;
    }
  }
  if (isAmbiguous) {
    return nullptr;
  }

  return best;
}

std::optional<int> LiteralTypes::candidateScore(const ObjectLitExpr *literal,
                                                const Type *member) const {
  if (!this->isPlainObjectMember(member) || !this->hasFittingNames(literal, member)) {
    return std::nullopt;
  }

  return this->memberScore(literal, member);
}

bool LiteralTypes::hasSpread(const ObjectLitExpr *literal) const {
  for (const ObjectLitExpr::Property &property : literal->props) {
    if (property.spread()) {
      return true;
    }
  }

  return false;
}

bool LiteralTypes::isPlainObjectMember(const Type *member) const {
  return member->isObject() && !member->isClass() && !member->isTuple() && !member->isRegExp() &&
         !member->isMatch() && !member->isAnyObject() && !member->isIncomplete();
}

bool LiteralTypes::hasFittingNames(const ObjectLitExpr *literal, const Type *member) const {
  for (const ObjectLitExpr::Property &property : literal->props) {
    if (member->field(property.name) == nullptr && member->overload(property.name) == nullptr) {
      return false;
    }
  }
  for (const Field &field : member->fields()) {
    if (!field.type->isOptional() && !this->hasProperty(literal, field.name)) {
      return false;
    }
  }

  return true;
}

bool LiteralTypes::hasProperty(const ObjectLitExpr *literal, llvm::StringRef name) const {
  for (const ObjectLitExpr::Property &property : literal->props) {
    if (property.name == name) {
      return true;
    }
  }

  return false;
}

std::optional<int> LiteralTypes::memberScore(const ObjectLitExpr *literal,
                                             const Type *member) const {
  int score = 0;
  for (const ObjectLitExpr::Property &property : literal->props) {
    auto gained = this->propertyScore(property, member);
    if (!gained.has_value()) {
      return std::nullopt;
    }
    score += *gained;
  }

  return score;
}

std::optional<int> LiteralTypes::propertyScore(const ObjectLitExpr::Property &property,
                                               const Type *member) const {
  const Field *field = member->field(property.name);
  const Type *tag = this->propertyTag(property, field);
  if (tag == nullptr) {
    return 0;
  }
  const Type *fieldType = field->type->nonOptional();
  bool isChoice = this->isLiteralSet(fieldType) || fieldType->isUnion();
  bool isConvertible = this->isTagConvertible(tag, fieldType);
  if (!isConvertible && isChoice) {
    return std::nullopt;
  }
  if (!isConvertible) {
    return 0;
  }
  bool hasAtoms = fieldType->isUnion() && fieldType->slotPart(UnionSlot::Atom) != nullptr;
  bool isTagged = this->isLiteralSet(fieldType) || hasAtoms;

  return isTagged ? 1 : 0;
}

bool LiteralTypes::isTagConvertible(const Type *tag, const Type *fieldType) const {
  if (TypeRelations::isImplicitlyConvertible(tag, fieldType)) {
    return true;
  }
  bool isIntLiteral = tag->isLiteral() && tag->literalBase()->isInt();

  return isIntLiteral && TypeRelations::isImplicitlyConvertible(tag->literalBase(), fieldType);
}

const Type *LiteralTypes::propertyTag(const ObjectLitExpr::Property &property,
                                      const Field *field) const {
  if (field == nullptr || property.value == nullptr) {
    return nullptr;
  }
  const Type *written = UnionCheckHelpers::writtenLiteral(property.value);
  if (written != nullptr) {
    return written;
  }
  const Type *enumTag = this->enumTag(property.value);
  if (enumTag != nullptr) {
    return enumTag;
  }

  return this->knownLiteralSet(property.value);
}

const Type *LiteralTypes::knownLiteralSet(const Expr *value) const {
  const VarSymbol *variable = this->namedVariable(value);
  if (variable == nullptr || variable->type == nullptr) {
    return nullptr;
  }
  const Type *known = this->typeIn(this->state.getFlowState().getFlow(), variable);
  if (known == nullptr || !this->isLiteralSet(known)) {
    return nullptr;
  }

  return known;
}

const VarSymbol *LiteralTypes::namedVariable(const Expr *expr) const {
  auto *name = dyn_cast<NameExpr>(expr);
  if (name == nullptr) {
    return nullptr;
  }
  auto entry = this->scopes->lookup(name->name);

  return entry.var();
}

const Type *LiteralTypes::typeIn(const Flow &flow, const VarSymbol *variable) const {
  for (const auto &[narrowed, type] : flow.narrowed) {
    if (narrowed == variable) {
      return type;
    }
  }
  if (variable->type->isOptional() && flow.defined.contains(variable)) {
    return variable->type->inner();
  }

  return variable->type;
}

std::optional<std::vector<std::string>> LiteralTypes::knownTexts(const Expr *arg) {
  auto texts = std::vector<std::string>();
  auto stack = std::vector<const Expr *>{arg};
  while (!stack.empty()) {
    const Expr *next = stack.back();
    stack.pop_back();
    auto lookup = this->lookupTexts(next);
    if (lookup.choice != nullptr) {
      stack.push_back(lookup.choice->elseExpr);
      stack.push_back(lookup.choice->thenExpr);
    } else if (lookup.texts.has_value()) {
      texts.insert(texts.end(), lookup.texts->begin(), lookup.texts->end());
    } else {
      return std::nullopt;
    }
  }

  return texts;
}

TextLookup LiteralTypes::lookupTexts(const Expr *arg) {
  auto typed = this->typedTexts(arg);
  if (typed.has_value()) {
    return TextLookup{.texts = typed, .choice = nullptr};
  }
  const Expr *bare = ExprParts::withoutImplicitCasts(arg);
  auto narrowed = this->narrowedTexts(bare);
  if (narrowed.has_value()) {
    return TextLookup{.texts = narrowed, .choice = nullptr};
  }
  const Expr *written = this->writtenValue(bare);
  auto *choice = dyn_cast<ConditionalExpr>(written);
  if (choice != nullptr) {
    return TextLookup{.texts = std::nullopt, .choice = choice};
  }
  auto text = this->writtenText(written);
  if (text.has_value()) {
    auto texts = std::vector<std::string>{ToUtf8::toUtf8(*text)};

    return TextLookup{.texts = texts, .choice = nullptr};
  }

  return TextLookup{.texts = this->typeTexts(written->type), .choice = nullptr};
}

std::optional<std::vector<std::string>> LiteralTypes::typedTexts(const Expr *arg) const {
  const Expr *expr = arg;
  auto texts = this->typeTexts(expr->type);
  auto *conversion = dyn_cast<CastExpr>(expr);
  while (!texts.has_value() && conversion != nullptr && conversion->isImplicit()) {
    expr = conversion->operand;
    texts = this->typeTexts(expr->type);
    conversion = dyn_cast<CastExpr>(expr);
  }

  return texts;
}

std::optional<std::vector<std::string>> LiteralTypes::narrowedTexts(const Expr *bare) const {
  const VarSymbol *variable = this->namedVariable(bare);
  if (variable == nullptr) {
    return std::nullopt;
  }
  const Type *flowType = this->typeIn(this->state.getFlowState().getFlow(), variable);

  return this->typeTexts(flowType);
}

const Expr *LiteralTypes::writtenValue(const Expr *bare) const {
  const VarSymbol *variable = this->namedVariable(bare);
  if (variable == nullptr) {
    return bare;
  }
  const auto *literal = this->state.getConstantStore().findConstLiteral(variable);
  if (literal == nullptr) {
    return bare;
  }

  return literal;
}

std::optional<std::vector<std::string>> LiteralTypes::typeTexts(const Type *type) const {
  if (type == nullptr) {
    return std::nullopt;
  }
  const Type *plain = type->nonOptional();
  auto texts = std::vector<std::string>();
  for (const Type *member : this->membersOf(plain)) {
    if (!member->isStringLiteral()) {
      return std::nullopt;
    }
    texts.push_back(ToUtf8::toUtf8(member->literalText()));
  }

  return texts;
}

}
