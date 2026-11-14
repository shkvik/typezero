#include "compiler/semantic/code/literals/object_literal_keys/object_literal_keys.h"
#include "compiler/semantic/types/union_check_helpers/union_check_helpers.h"

using llvm::cast;
using llvm::dyn_cast;
using llvm::dyn_cast_or_null;

namespace tz {

ObjectLiteralKeys::ObjectLiteralKeys(SemanticState &owner)
    : state(owner) {}

void ObjectLiteralKeys::checkObjectLiteral(const CheckObjectLiteral &task) {
  ObjectLitExpr *literal = task.literal;
  const Type *wanted = task.expected != nullptr ? task.expected->nonOptional() : nullptr;
  if (wanted != nullptr && wanted->isJson()) {
    return this->state.walk.tasks.pushTask(CheckJsonObjectLit{.literal = literal});
  }
  if (wanted != nullptr && wanted->isRecord()) {
    return this->startRecordLit(literal, wanted);
  }
  auto draft = ObjectLitDraft{
      .literal = literal,
      .wanted = wanted,
      .target = this->isUsableTarget(wanted) ? wanted : nullptr,
      .named = std::vector<bool>(literal->props.size(), true),
      .isTyped = true,
      .isInferred = false,
      .fields = {},
      .index = 0,
  };

  return this->nextPropertyKey(std::move(draft));
}

void ObjectLiteralKeys::nextPropertyKey(ObjectLitDraft draft) {
  ObjectLitExpr *literal = draft.literal;
  while (draft.index < literal->props.size()) {
    ObjectLitExpr::Property &property = literal->props[draft.index];
    draft.named[draft.index] = !property.spread();
    auto plain = this->checkPropertyPlain(property);
    if (plain.isErr()) {
      this->state.diagnostics.error(property.loc, plain.error().message);
      property.value = cast<AssignExpr>(property.value)->value;
      property.form = ValueProperty{};
      draft.isTyped = false;
    }
    ComputedProperty *computed = property.form.as<ComputedProperty>();
    if (computed != nullptr) {
      this->state.walk.tasks.pushTask(AfterComputedKey{.draft = std::move(draft)});
      return this->state.walk.tasks.pushTask(
          CheckExprSlot{.slot = &computed->key, .expected = nullptr});
    }
    draft.index += 1;
  }

  return this->afterPropertyKeys(std::move(draft));
}

void ObjectLiteralKeys::afterComputedKey(const AfterComputedKey &task) {
  auto draft = task.draft;
  ObjectLitExpr::Property &property = draft.literal->props[draft.index];
  draft.index += 1;
  const Type *keyType = property.key()->type;
  auto name = this->state.code.constants.constantKey(property.key());
  if (name.has_value()) {
    property.name = this->state.ast.keepName(std::move(*name));
    this->state.code.literals.addFieldName(property.name);
    return this->nextPropertyKey(std::move(draft));
  }
  draft.named[draft.index - 1] = false;
  if (this->isLiteralKeys(keyType)) {
    property.form.as<ComputedProperty>()->keyed = true;
    return this->nextPropertyKey(std::move(draft));
  }
  draft.isTyped = false;
  if (!keyType->isError()) {
    auto key = this->checkComputedKey(keyType);
    if (key.isErr()) {
      this->state.diagnostics.error(property.key()->loc(), key.error().message);
    }
  }

  return this->nextPropertyKey(std::move(draft));
}

void ObjectLiteralKeys::afterPropertyKeys(ObjectLitDraft draft) {
  ObjectLitExpr *literal = draft.literal;
  for (size_t index = 0; index < literal->props.size(); ++index) {
    auto fresh = this->checkUniqueName(literal, index, draft.named);
    if (fresh.isErr()) {
      this->state.diagnostics.error(literal->props[index].loc, fresh.error().message);
      draft.isTyped = false;
    }
  }
  bool isUnionWanted = draft.wanted != nullptr && draft.wanted->isUnion();
  if (!isUnionWanted) {
    return this->continueObjectLit(std::move(draft));
  }
  this->state.walk.tasks.pushTask(AfterUnionNeeds{.draft = std::move(draft)});
  for (EnumDecl *enumDecl : this->referencedEnums(literal)) {
    this->state.walk.tasks.pushTask(ResolveEnum{.enumDecl = enumDecl});
  }
}

std::vector<EnumDecl *> ObjectLiteralKeys::referencedEnums(const ObjectLitExpr *literal) const {
  auto enums = std::vector<EnumDecl *>();
  for (const ObjectLitExpr::Property &property : literal->props) {
    auto *member = dyn_cast_or_null<MemberExpr>(property.value);
    auto *owner = member != nullptr ? dyn_cast<NameExpr>(member->object) : nullptr;
    EnumDecl *enumDecl =
        owner != nullptr ? this->state.walk.scopes.findName(owner->name).enumDecl() : nullptr;
    if (enumDecl != nullptr) {
      enums.push_back(enumDecl);
    }
  }

  return enums;
}

void ObjectLiteralKeys::afterUnionNeeds(const AfterUnionNeeds &task) {
  auto draft = task.draft;
  const auto *member = this->literalMember(draft.literal, draft.wanted);
  draft.target = this->isUsableTarget(member) ? member : nullptr;
  if (draft.target != nullptr) {
    return this->continueObjectLit(std::move(draft));
  }
  for (const Type *memberType : draft.wanted->members()) {
    if (memberType->isRecord()) {
      return this->startRecordLit(draft.literal, memberType);
    }
  }

  return this->continueObjectLit(std::move(draft));
}

void ObjectLiteralKeys::continueObjectLit(ObjectLitDraft draft) {
  ObjectLitExpr *literal = draft.literal;
  if (draft.target != nullptr) {
    this->walkThisMethods(literal, draft.target);
  }
  bool isKeyedOnly =
      draft.target == nullptr && !literal->props.empty() && this->isAllKeyed(literal);
  draft.index = 0;
  if (isKeyedOnly) {
    return this->state.walk.tasks.pushTask(NextInferredKey{.draft = std::move(draft)});
  }

  return this->state.walk.tasks.pushTask(NextKeyedProperty{.draft = std::move(draft)});
}

void ObjectLiteralKeys::startRecordLit(ObjectLitExpr *literal, const Type *record) {
  auto draft = RecordDraft{.literal = literal, .record = record, .hasFailed = false, .index = 0};

  return this->state.walk.tasks.pushTask(NextRecordProperty{.draft = draft});
}

ObjectLiteralKeys::ResultPropertyHasValue
ObjectLiteralKeys::checkPropertyPlain(const ObjectLitExpr::Property &property) const {
  if (property.defaulted()) {
    auto message = ErrorTextObjectLiteral::defaulted(property.name);

    return err(CodeLiteralsRegistryPropertyPlainError{
        .code = CodeLiteralsRegistryPropertyPlainErrorCode::DEFAULT_VALUE,
        .message = message,
    });
  }

  return ok(&property);
}

ObjectLiteralKeys::ResultComputedKeyKnown
ObjectLiteralKeys::checkComputedKey(const Type *keyType) const {
  if (keyType->nonOptional()->isNumeric()) {
    auto message = ErrorTextObjectLiteral::numberKey();

    return err(CodeLiteralsRegistryComputedKeyError{
        .code = CodeLiteralsRegistryComputedKeyErrorCode::NUMBER_KEY,
        .message = message,
    });
  }
  if (keyType->isString()) {
    auto message = ErrorTextObjectLiteral::runtimeKey();

    return err(CodeLiteralsRegistryComputedKeyError{
        .code = CodeLiteralsRegistryComputedKeyErrorCode::RUNTIME_KEY,
        .message = message,
    });
  }
  auto message = ErrorTextObjectLiteral::keyType(keyType->name());

  return err(CodeLiteralsRegistryComputedKeyError{
      .code = CodeLiteralsRegistryComputedKeyErrorCode::NOT_A_STRING,
      .message = message,
  });
}

ObjectLiteralKeys::ResultFieldNameIsFresh
ObjectLiteralKeys::checkUniqueName(const ObjectLitExpr *literal, size_t index,
                                   llvm::ArrayRef<bool> named) const {
  if (!named[index]) {
    return ok(&literal->props[index]);
  }
  for (size_t earlier = 0; earlier < index; ++earlier) {
    if (named[earlier] && literal->props[earlier].name == literal->props[index].name) {
      auto message = ErrorTextObjectLiteral::duplicate(literal->props[index].name);

      return err(CodeLiteralsRegistryUniqueNameError{
          .code = CodeLiteralsRegistryUniqueNameErrorCode::DUPLICATE_FIELD,
          .message = message,
      });
    }
  }

  return ok(&literal->props[index]);
}

bool ObjectLiteralKeys::isUsableTarget(const Type *type) const {
  return type != nullptr && type->isObject() && !type->isIncomplete() && !type->isTuple() &&
         !type->isClass() && !type->isRegExp() && !type->isMatch() && !type->isDate() &&
         !type->isIntl() && !type->isBytes() && !type->isCrypto() && !type->isAnyObject();
}

bool ObjectLiteralKeys::isLiteralKeys(const Type *keyType) const {
  llvm::ArrayRef<const Type *> names =
      keyType->isUnion() ? keyType->members() : llvm::ArrayRef<const Type *>(keyType);
  for (const Type *nameType : names) {
    if (!nameType->isStringLiteral()) {
      return false;
    }
  }

  return true;
}

void ObjectLiteralKeys::walkThisMethods(ObjectLitExpr *literal, const Type *target) const {
  for (ObjectLitExpr::Property &property : literal->props) {
    const Field *wanted = property.method() ? target->field(property.name) : nullptr;
    bool isBound = wanted != nullptr && wanted->type->nonOptional()->isFunction() &&
                   wanted->type->nonOptional()->hasThis();
    if (isBound) {
      property.form = ValueProperty{};
    }
  }
}

bool ObjectLiteralKeys::isAllKeyed(const ObjectLitExpr *literal) const {
  for (const ObjectLitExpr::Property &property : literal->props) {
    if (!property.keyed()) {
      return false;
    }
  }

  return true;
}

bool ObjectLiteralKeys::isLiteralSet(const Type *type) const {
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

const Type *ObjectLiteralKeys::enumTag(const Expr *expr) const {
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

const Type *ObjectLiteralKeys::literalMember(const ObjectLitExpr *literal,
                                             const Type *target) const {
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

std::optional<int> ObjectLiteralKeys::candidateScore(const ObjectLitExpr *literal,
                                                     const Type *member) const {
  if (!this->isPlainObjectMember(member) || !this->hasFittingNames(literal, member)) {
    return std::nullopt;
  }

  return this->memberScore(literal, member);
}

bool ObjectLiteralKeys::hasObjectSpread(const ObjectLitExpr *literal) const {
  for (const ObjectLitExpr::Property &property : literal->props) {
    if (property.spread()) {
      return true;
    }
  }

  return false;
}

bool ObjectLiteralKeys::isPlainObjectMember(const Type *member) const {
  return member->isObject() && !member->isClass() && !member->isTuple() && !member->isRegExp() &&
         !member->isMatch() && !member->isAnyObject() && !member->isIncomplete();
}

bool ObjectLiteralKeys::hasFittingNames(const ObjectLitExpr *literal, const Type *member) const {
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

bool ObjectLiteralKeys::hasProperty(const ObjectLitExpr *literal, llvm::StringRef name) const {
  for (const ObjectLitExpr::Property &property : literal->props) {
    if (property.name == name) {
      return true;
    }
  }

  return false;
}

std::optional<int> ObjectLiteralKeys::memberScore(const ObjectLitExpr *literal,
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

std::optional<int> ObjectLiteralKeys::propertyScore(const ObjectLitExpr::Property &property,
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

bool ObjectLiteralKeys::isTagConvertible(const Type *tag, const Type *fieldType) const {
  if (this->state.types.relations.isImplicitlyConvertible(tag, fieldType)) {
    return true;
  }
  bool isIntLiteral = tag->isLiteral() && tag->literalBase()->isInt();

  return isIntLiteral &&
         this->state.types.relations.isImplicitlyConvertible(tag->literalBase(), fieldType);
}

const Type *ObjectLiteralKeys::propertyTag(const ObjectLitExpr::Property &property,
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

const Type *ObjectLiteralKeys::knownLiteralSet(const Expr *value) const {
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

const VarSymbol *ObjectLiteralKeys::namedVariable(const Expr *expr) const {
  auto *name = dyn_cast<NameExpr>(expr);
  if (name == nullptr) {
    return nullptr;
  }
  auto entry = this->state.walk.scopes.findName(name->name);

  return entry.var();
}

}
