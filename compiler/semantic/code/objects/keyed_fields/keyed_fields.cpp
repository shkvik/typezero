#include "compiler/semantic/code/objects/keyed_fields/keyed_fields.h"
#include "compiler/program/library/object/object_member_table/object_member_table.h"
#include "compiler/program/to_utf8/to_utf8.h"
#include "compiler/semantic/flow/expr_parts/expr_parts.h"
#include "llvm/ADT/STLExtras.h"

using llvm::dyn_cast;

namespace tz {

KeyedFields::KeyedFields(SemanticState &owner)
    : state(owner) {}

void KeyedFields::checkKeyedField(const CheckKeyedField &task) {
  IndexExpr *index = task.index;
  const Type *type = index->object->type;
  index->type = Type::error();
  if (type->isTuple()) {
    this->state.walk.tasks.pushTask(AfterTupleKey{.index = index, .access = task.access});
    return this->state.walk.tasks.pushTask(
        CheckExprSlot{.slot = &index->index, .expected = Type::intTy()});
  }
  this->state.walk.tasks.pushTask(AfterObjectKey{.index = index, .access = task.access});

  return this->state.walk.tasks.pushTask(CheckExprSlot{.slot = &index->index, .expected = nullptr});
}

void KeyedFields::afterTupleKey(const AfterTupleKey &task) {
  IndexExpr *index = task.index;
  const Type *type = index->object->type;
  auto position = this->checkTupleKeyName(index, type);
  if (position.isErr()) {
    return this->reportKeyed(index, position.error());
  }

  return this->pushNamedField(index, type, position.value(), task.access);
}

void KeyedFields::afterObjectKey(const AfterObjectKey &task) {
  IndexExpr *index = task.index;
  const Type *type = index->object->type;
  auto key = this->state.code.constants.constantKey(index->index);
  const Type *texts = key.has_value() ? nullptr : this->constTextType(index->index);
  bool isTextKeys = texts != nullptr && this->keyedFieldType(type, texts) != nullptr;
  if (isTextKeys) {
    this->state.walk.tasks.pushTask(ObjectKeyReady{.index = index, .access = task.access});
    return this->state.walk.tasks.pushTask(ConvertSlot{.slot = &index->index, .to = texts});
  }
  auto ready = ObjectKeyReady{.index = index, .access = task.access};

  return this->objectKeyReady(ready);
}

void KeyedFields::objectKeyReady(const ObjectKeyReady &task) {
  IndexExpr *index = task.index;
  const Type *type = index->object->type;
  auto key = this->state.code.constants.constantKey(index->index);
  const Type *keyType = index->index->type;
  if (keyType->isError()) {
    return this->state.walk.tasks.pushChecked(index);
  }
  const Type *field = key.has_value() ? nullptr : this->keyedFieldType(type, keyType);
  if (field != nullptr && task.access == KeyedAccess::WRITE) {
    auto written = this->checkKeySetWrite(index, type, keyType, field);
    if (written.isErr()) {
      return this->reportKeyed(index, written.error());
    }
    return this->state.walk.tasks.pushChecked(written.value());
  }
  if (field != nullptr) {
    auto *keyed = this->keySetIndex(index, field);
    return this->state.walk.tasks.pushChecked(keyed);
  }
  auto named = this->checkObjectKeyName(type, keyType, key);
  if (named.isErr()) {
    return this->reportKeyed(index, named.error());
  }

  return this->pushNamedField(index, type, named.value(), task.access);
}

void KeyedFields::pushNamedField(IndexExpr *index, const Type *type, const std::string &name,
                                 KeyedAccess access) {
  bool isAssigned = this->state.flow.assignments.isFieldAssigned(name);
  bool isUnassignedWrite = access == KeyedAccess::WRITE && !isAssigned;
  if (isUnassignedWrite && type->isTuple()) {
    auto message = ErrorTextKeyedField::tupleSet();
    return this->reportKeyed(
        index, CodeObjectsRegistryCheckKeyedFieldError{
                   .code = CodeObjectsRegistryCheckKeyedFieldErrorCode::TUPLE_ELEMENT_BY_NUMBER,
                   .message = message,
               });
  }
  if (isUnassignedWrite) {
    auto message = ErrorTextKeyedField::keySet();
    return this->reportKeyed(
        index, CodeObjectsRegistryCheckKeyedFieldError{
                   .code = CodeObjectsRegistryCheckKeyedFieldErrorCode::FIELD_KEY_NOT_LITERAL,
                   .message = message,
               });
  }
  auto field = this->state.ast.keepName(name);
  index->form = NamedFieldIndex{.field = field};
  auto *member = this->state.ast.make<MemberExpr>(index->loc(), index->object, field);
  if (access == KeyedAccess::WRITE) {
    return this->state.walk.tasks.pushChecked(member);
  }

  return this->state.walk.tasks.pushTask(CheckField{.member = member});
}

void KeyedFields::reportKeyed(IndexExpr *index,
                              const CodeObjectsRegistryCheckKeyedFieldError &error) {
  this->state.walk.tasks.pushChecked(index);
  if (error.code == CodeObjectsRegistryCheckKeyedFieldErrorCode::ALREADY_REPORTED) {
    return;
  }
  llvm::SMLoc at = this->isWrongKey(error.code) ? index->index->loc() : index->loc();

  return this->state.diagnostics.error(at, error.message);
}

const Type *KeyedFields::constTextType(const Expr *checked) const {
  const auto *name = dyn_cast<NameExpr>(checked);
  bool isTextConstant = name != nullptr && name->var() != nullptr && name->var()->literalConst &&
                        checked->type->isString();
  if (!isTextConstant) {
    return nullptr;
  }
  const auto *written = this->state.code.constants.findConstLiteral(name->var());
  if (written == nullptr) {
    return nullptr;
  }
  auto literals = std::vector<const Type *>();
  for (const Expr *branch : ExprParts::choiceBranches(written)) {
    auto text = this->writtenText(branch);
    if (!text.has_value()) {
      return nullptr;
    }
    const Type *literal = Type::stringLiteral(*text);
    if (!llvm::is_contained(literals, literal)) {
      literals.push_back(literal);
    }
  }

  return Type::unionOf(literals);
}

std::optional<std::u16string> KeyedFields::writtenText(const Expr *expr) const {
  const auto *literal = dyn_cast<StringLitExpr>(expr);
  const auto *templateExpr = dyn_cast<TemplateExpr>(expr);
  if (literal != nullptr) {
    return literal->value;
  }
  if (templateExpr != nullptr && templateExpr->exprs.empty()) {
    return templateExpr->quasis[0];
  }

  return std::nullopt;
}

KeyedFields::ResultKeyedFieldName KeyedFields::checkTupleKeyName(IndexExpr *index,
                                                                 const Type *type) {
  const Type *keyType = index->index->type;
  if (keyType->isError()) {
    return err(CodeObjectsRegistryCheckKeyedFieldError{
        .code = CodeObjectsRegistryCheckKeyedFieldErrorCode::ALREADY_REPORTED,
        .message = "",
    });
  }
  std::optional<int64_t> position =
      keyType->isInteger() ? this->state.code.constants.intConstant(index->index) : std::nullopt;
  size_t count = type->fields().size();
  if (!keyType->isInteger()) {
    auto message = ErrorTextKeyedField::tupleKey(keyType->name());

    return err(CodeObjectsRegistryCheckKeyedFieldError{
        .code = CodeObjectsRegistryCheckKeyedFieldErrorCode::TUPLE_KEY_NOT_NUMBER,
        .message = message,
    });
  }
  if (!position.has_value()) {
    auto message = ErrorTextKeyedField::tupleConst();

    return err(CodeObjectsRegistryCheckKeyedFieldError{
        .code = CodeObjectsRegistryCheckKeyedFieldErrorCode::TUPLE_KEY_NOT_CONSTANT,
        .message = message,
    });
  }
  if (*position < 0 || uint64_t(*position) >= count) {
    auto message = ErrorTextKeyedField::tupleRange(type->name(), count, *position);

    return err(CodeObjectsRegistryCheckKeyedFieldError{
        .code = CodeObjectsRegistryCheckKeyedFieldErrorCode::TUPLE_KEY_OUT_OF_RANGE,
        .message = message,
    });
  }

  return ok(std::to_string(*position));
}

KeyedFields::ResultKeyedFieldRead KeyedFields::checkKeySetWrite(IndexExpr *index, const Type *type,
                                                                const Type *keyType,
                                                                const Type *field) {
  llvm::ArrayRef<const Type *> names =
      keyType->isUnion() ? keyType->members() : llvm::ArrayRef<const Type *>(keyType);
  bool isKnown = this->isAllAssigned(names);
  if (this->hasReadonly(type, names)) {
    auto message = ErrorTextKeyedField::readonly();

    return err(CodeObjectsRegistryCheckKeyedFieldError{
        .code = CodeObjectsRegistryCheckKeyedFieldErrorCode::READONLY_THROUGH_KEY,
        .message = message,
    });
  }
  if ((field->isOptional() || field->isUnion() || field->isLiteral()) && !isKnown) {
    auto message = ErrorTextKeyedField::byName(field->name());

    return err(CodeObjectsRegistryCheckKeyedFieldError{
        .code = CodeObjectsRegistryCheckKeyedFieldErrorCode::ASSIGNED_BY_NAME,
        .message = message,
    });
  }
  for (const Type *nameType : names) {
    auto text = ToUtf8::toUtf8(nameType->literalText());
    auto kept = this->state.ast.keepName(text);
    this->state.walk.flow.forgetFieldsNamed(kept);
  }
  auto *keyed = this->keySetIndex(index, field);

  return ok(keyed);
}

bool KeyedFields::isInherited(llvm::StringRef name) const {
  auto member = ObjectMemberTable::find(name);

  return member.has_value();
}

bool KeyedFields::isWrongKey(CodeObjectsRegistryCheckKeyedFieldErrorCode code) const {
  switch (code) {
    case CodeObjectsRegistryCheckKeyedFieldErrorCode::TUPLE_KEY_NOT_NUMBER:
    case CodeObjectsRegistryCheckKeyedFieldErrorCode::TUPLE_KEY_OUT_OF_RANGE:
    case CodeObjectsRegistryCheckKeyedFieldErrorCode::KEY_NOT_STRING:
    case CodeObjectsRegistryCheckKeyedFieldErrorCode::TUPLE_ELEMENT_BY_NUMBER:
    case CodeObjectsRegistryCheckKeyedFieldErrorCode::FIELD_KEY_NOT_LITERAL:
      return true;
    case CodeObjectsRegistryCheckKeyedFieldErrorCode::ALREADY_REPORTED:
    case CodeObjectsRegistryCheckKeyedFieldErrorCode::TUPLE_KEY_NOT_CONSTANT:
    case CodeObjectsRegistryCheckKeyedFieldErrorCode::READONLY_THROUGH_KEY:
    case CodeObjectsRegistryCheckKeyedFieldErrorCode::ASSIGNED_BY_NAME:
    case CodeObjectsRegistryCheckKeyedFieldErrorCode::NO_NUMBERED_FIELDS:
    case CodeObjectsRegistryCheckKeyedFieldErrorCode::KEY_NOT_CONSTANT:
    case CodeObjectsRegistryCheckKeyedFieldErrorCode::NO_SUCH_FIELD:
    case CodeObjectsRegistryCheckKeyedFieldErrorCode::FIELD_NOT_FOUND:
      return false;
  }

  return false;
}

const Type *KeyedFields::keyedFieldType(const Type *type, const Type *keyType) const {
  if (type->isTuple() || !type->isObject()) {
    return nullptr;
  }
  const ClassDecl *classDecl = type->isClass() ? this->classOf(type) : nullptr;
  if (type->isClass() && classDecl == nullptr) {
    return nullptr;
  }
  llvm::ArrayRef<const Type *> names =
      keyType->isUnion() ? keyType->members() : llvm::ArrayRef<const Type *>(keyType);
  const Type *common = nullptr;
  for (const Type *nameType : names) {
    if (!nameType->isStringLiteral()) {
      return nullptr;
    }
    auto text = ToUtf8::toUtf8(nameType->literalText());
    const auto *found = type->field(text);
    bool isHidden = classDecl != nullptr && !this->isPublicField(classDecl, text);
    if (isHidden || found == nullptr || (common != nullptr && found->type != common)) {
      return nullptr;
    }
    common = found->type;
  }

  return common;
}

bool KeyedFields::isPublicField(const ClassDecl *classDecl, const std::string &name) const {
  const auto *field = classDecl->findField(name);

  return field != nullptr && !field->isStatic() && field->access == Access::Public;
}

KeyedFields::ResultKeyedFieldName
KeyedFields::checkObjectKeyName(const Type *type, const Type *keyType,
                                const std::optional<std::string> &key) const {
  if (!key.has_value() && keyType->nonOptional()->isNumeric()) {
    auto message = ErrorTextKeyedField::numbered(type->name());

    return err(CodeObjectsRegistryCheckKeyedFieldError{
        .code = CodeObjectsRegistryCheckKeyedFieldErrorCode::NO_NUMBERED_FIELDS,
        .message = message,
    });
  }
  if (!key.has_value() && keyType->isString()) {
    auto message = ErrorTextKeyedField::runtimeKey(type->name());

    return err(CodeObjectsRegistryCheckKeyedFieldError{
        .code = CodeObjectsRegistryCheckKeyedFieldErrorCode::KEY_NOT_CONSTANT,
        .message = message,
    });
  }
  if (!key.has_value()) {
    auto message = ErrorTextKeyedField::keyString(keyType->name());

    return err(CodeObjectsRegistryCheckKeyedFieldError{
        .code = CodeObjectsRegistryCheckKeyedFieldErrorCode::KEY_NOT_STRING,
        .message = message,
    });
  }
  if (!type->isClass() && type->field(*key) == nullptr) {
    bool isInherited = this->isInherited(*key);
    auto message = ErrorTextKeyedField::noField(*key, type->name(), isInherited);

    return err(CodeObjectsRegistryCheckKeyedFieldError{
        .code = CodeObjectsRegistryCheckKeyedFieldErrorCode::NO_SUCH_FIELD,
        .message = message,
    });
  }

  return ok(*key);
}

Expr *KeyedFields::keySetIndex(IndexExpr *index, const Type *field) const {
  index->form = KeyedFieldIndex{};
  index->type = field;

  return index;
}

bool KeyedFields::isAllAssigned(llvm::ArrayRef<const Type *> names) const {
  for (const Type *nameType : names) {
    auto text = ToUtf8::toUtf8(nameType->literalText());
    if (!this->state.flow.assignments.isFieldAssigned(text)) {
      return false;
    }
  }

  return true;
}

bool KeyedFields::hasReadonly(const Type *type, llvm::ArrayRef<const Type *> names) const {
  for (const Type *nameType : names) {
    auto text = ToUtf8::toUtf8(nameType->literalText());
    if (type->field(text)->readonly) {
      return true;
    }
  }

  return false;
}

ClassDecl *KeyedFields::classOf(const Type *type) const {
  if (type == nullptr) {
    return nullptr;
  }
  const auto *classDecl = type->classDecl();

  return const_cast<ClassDecl *>(classDecl);
}

}
