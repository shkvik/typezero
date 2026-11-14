#include "compiler/semantic/inference_v2/destinations/literal_members/literal_members.h"
#include "compiler/semantic/semantic_state/v2/semantic_state.h"
#include "compiler/semantic/types/union_check_helpers/union_check_helpers.h"

using llvm::dyn_cast;

namespace tz {

LiteralMembers::LiteralMembers(SemanticState &owner)
    : state(owner) {}

const Type *LiteralMembers::literalMember(const ObjectLitExpr *literal, const Type *target) const {
  if (this->hasObjectSpread(literal)) {
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

std::optional<int> LiteralMembers::candidateScore(const ObjectLitExpr *literal,
                                                  const Type *member) const {
  if (!this->isPlainObjectMember(member) || !this->hasFittingNames(literal, member)) {
    return std::nullopt;
  }

  return this->memberScore(literal, member);
}

bool LiteralMembers::hasObjectSpread(const ObjectLitExpr *literal) const {
  for (const ObjectLitExpr::Property &property : literal->props) {
    if (property.spread()) {
      return true;
    }
  }

  return false;
}

bool LiteralMembers::isPlainObjectMember(const Type *member) const {
  return member->isObject() && !member->isClass() && !member->isTuple() && !member->isRegExp() &&
         !member->isMatch() && !member->isAnyObject() && !member->isIncomplete();
}

bool LiteralMembers::hasFittingNames(const ObjectLitExpr *literal, const Type *member) const {
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

bool LiteralMembers::hasProperty(const ObjectLitExpr *literal, llvm::StringRef name) const {
  for (const ObjectLitExpr::Property &property : literal->props) {
    if (property.name == name) {
      return true;
    }
  }

  return false;
}

std::optional<int> LiteralMembers::memberScore(const ObjectLitExpr *literal,
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

std::optional<int> LiteralMembers::propertyScore(const ObjectLitExpr::Property &property,
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

bool LiteralMembers::isTagConvertible(const Type *tag, const Type *fieldType) const {
  if (this->state.types.relations.isImplicitlyConvertible(tag, fieldType)) {
    return true;
  }
  bool isIntLiteral = tag->isLiteral() && tag->literalBase()->isInt();

  return isIntLiteral &&
         this->state.types.relations.isImplicitlyConvertible(tag->literalBase(), fieldType);
}

const Type *LiteralMembers::propertyTag(const ObjectLitExpr::Property &property,
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

bool LiteralMembers::isLiteralSet(const Type *type) const {
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

const Type *LiteralMembers::enumTag(const Expr *expr) const {
  auto *member = dyn_cast<MemberExpr>(expr);
  if (member == nullptr) {
    return nullptr;
  }
  auto *owner = dyn_cast<NameExpr>(member->object);
  if (owner == nullptr) {
    return nullptr;
  }
  auto entry = this->state.walk.scopes.findName(owner->name);
  EnumDecl *enumDecl = entry.enumDecl();
  if (enumDecl == nullptr) {
    return nullptr;
  }
  const auto *enumMember = enumDecl->find(member->name);
  if (enumMember == nullptr) {
    return nullptr;
  }
  if (enumMember->isString) {
    return Type::stringLiteral(enumMember->text);
  }

  return Type::numberLiteral(double(enumMember->number), Type::intTy());
}

const Type *LiteralMembers::knownLiteralSet(const Expr *value) const {
  const VarSymbol *variable = this->namedVariable(value);
  if (variable == nullptr || variable->type == nullptr) {
    return nullptr;
  }
  const Type *known = this->state.walk.flow.findVariableType(variable);
  if (known == nullptr || !this->isLiteralSet(known)) {
    return nullptr;
  }

  return known;
}

const VarSymbol *LiteralMembers::namedVariable(const Expr *expr) const {
  auto *name = dyn_cast<NameExpr>(expr);
  if (name == nullptr) {
    return nullptr;
  }
  auto entry = this->state.walk.scopes.findName(name->name);

  return entry.var();
}

}
