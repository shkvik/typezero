#include "compiler/semantic/bodies/objects/keyed_field_checker/keyed_field_checker.h"
#include "compiler/program/library/object/object_member_table/object_member_table.h"
#include "compiler/program/to_utf8/to_utf8.h"
#include "compiler/semantic/bodies/classes/class_bases/class_bases.h"
#include "compiler/semantic/bodies/expr/expr_checker/expr_checker.h"
#include "compiler/semantic/bodies/objects/object_checker/object_checker.h"
#include "compiler/semantic/declare/class_declarer/class_declarer.h"
#include "compiler/semantic/declare/constant_folder/constant_folder.h"
#include "compiler/semantic/flow/expr_parts/expr_parts.h"
#include "compiler/semantic/flow/field_facts/field_facts.h"
#include "compiler/semantic/semantic_parts/semantic_parts.h"
#include "compiler/semantic/types/literal_types/literal_types.h"
#include "compiler/semantic/types/type_relations/type_relations.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/ADT/StringExtras.h"
#include "llvm/Support/ConvertUTF.h"

using llvm::dyn_cast;

namespace tz {

KeyedFieldChecker::KeyedFieldChecker(SemanticState &owner, SemanticParts &all)
    : state(owner) {
  this->fieldFacts = &all.fieldFacts;
  this->classBases = &all.classBases;
  this->exprChecker = &all.exprChecker;
  this->literalTypes = &all.literalTypes;
  this->objectChecker = &all.objectChecker;
  this->typeRelations = &all.typeRelations;
  this->classDeclarer = &all.classDeclarer;
  this->constantFolder = &all.constantFolder;
}

bool KeyedFieldChecker::isInherited(llvm::StringRef name) const {
  auto member = ObjectMemberTable::find(name);

  return member.has_value();
}

bool KeyedFieldChecker::isWrongKey(KeyedFieldCheckerCheckKeyedFieldErrorCode code) const {
  switch (code) {
    case KeyedFieldCheckerCheckKeyedFieldErrorCode::TUPLE_KEY_NOT_NUMBER:
    case KeyedFieldCheckerCheckKeyedFieldErrorCode::TUPLE_KEY_OUT_OF_RANGE:
    case KeyedFieldCheckerCheckKeyedFieldErrorCode::KEY_NOT_STRING:
    case KeyedFieldCheckerCheckKeyedFieldErrorCode::TUPLE_ELEMENT_BY_NUMBER:
    case KeyedFieldCheckerCheckKeyedFieldErrorCode::FIELD_KEY_NOT_LITERAL:
      return true;
    case KeyedFieldCheckerCheckKeyedFieldErrorCode::ALREADY_REPORTED:
    case KeyedFieldCheckerCheckKeyedFieldErrorCode::TUPLE_KEY_NOT_CONSTANT:
    case KeyedFieldCheckerCheckKeyedFieldErrorCode::READONLY_THROUGH_KEY:
    case KeyedFieldCheckerCheckKeyedFieldErrorCode::ASSIGNED_BY_NAME:
    case KeyedFieldCheckerCheckKeyedFieldErrorCode::NO_NUMBERED_FIELDS:
    case KeyedFieldCheckerCheckKeyedFieldErrorCode::KEY_NOT_CONSTANT:
    case KeyedFieldCheckerCheckKeyedFieldErrorCode::NO_SUCH_FIELD:
    case KeyedFieldCheckerCheckKeyedFieldErrorCode::FIELD_NOT_FOUND:
      return false;
  }

  return false;
}

std::optional<std::string> KeyedFieldChecker::constantKey(const Expr *key) const {
  const Expr *plain = ExprParts::withoutImplicitCasts(key);
  auto *literal = dyn_cast<StringLitExpr>(plain);
  if (literal != nullptr) {
    return ToUtf8::toUtf8(literal->value);
  }
  auto *templateExpr = dyn_cast<TemplateExpr>(plain);
  if (templateExpr != nullptr && templateExpr->exprs.empty()) {
    return ToUtf8::toUtf8(templateExpr->quasis[0]);
  }
  auto *name = dyn_cast<NameExpr>(plain);
  if (name == nullptr || name->var() == nullptr) {
    return std::nullopt;
  }

  return this->state.getConstantStore().findConstKey(name->var());
}

std::vector<std::string> KeyedFieldChecker::keyNames(const Expr *key) const {
  auto *literal = dyn_cast<StringLitExpr>(key);
  auto *templateExpr = dyn_cast<TemplateExpr>(key);
  auto *number = dyn_cast<IntLitExpr>(key);
  auto *name = dyn_cast<NameExpr>(key);
  auto *member = dyn_cast<MemberExpr>(key);
  if (literal != nullptr) {
    return {ToUtf8::toUtf8(literal->value)};
  }
  if (templateExpr != nullptr && templateExpr->exprs.empty()) {
    return {ToUtf8::toUtf8(templateExpr->quasis[0])};
  }
  if (number != nullptr) {
    return {std::to_string(number->value)};
  }
  if (name != nullptr) {
    return this->state.getInventoryStore().getStringConstants(name->name);
  }
  if (member != nullptr) {
    return this->memberConstants(member);
  }

  return {};
}

std::vector<std::string> KeyedFieldChecker::memberConstants(const MemberExpr *member) const {
  auto *object = dyn_cast<NameExpr>(member->object);
  if (object == nullptr) {
    return {};
  }
  std::string path = (object->name + "." + member->name).str();

  return this->state.getInventoryStore().getStringConstants(path);
}

const Type *KeyedFieldChecker::keyedFieldType(const Type *type, const Type *keyType) const {
  if (type->isTuple() || !type->isObject()) {
    return nullptr;
  }
  const ClassDecl *classDecl = type->isClass() ? this->classDeclarer->classOf(type) : nullptr;
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

bool KeyedFieldChecker::isPublicField(const ClassDecl *classDecl, const std::string &name) const {
  const auto *field = this->classBases->findField(classDecl, name, nullptr);

  return field != nullptr && !field->isStatic() && field->access == Access::Public;
}

KeyedFieldChecker::ResultKeyedFieldRead KeyedFieldChecker::checkKeyedField(IndexExpr *index,
                                                                           KeyedAccess access) {
  const Type *type = index->object->type;
  index->type = Type::error();
  if (type->isTuple()) {
    return this->checkTupleField(index, type, access);
  }
  index->index = this->exprChecker->checkExpr(index->index);
  auto key = this->constantKey(index->index);
  const Type *texts =
      key.has_value() ? nullptr : this->literalTypes->constTextType(index->index, nullptr);
  if (texts != nullptr && this->keyedFieldType(type, texts) != nullptr) {
    index->index = this->typeRelations->convert(index->index, texts);
  }
  const Type *keyType = index->index->type;
  if (keyType->isError()) {
    return err(KeyedFieldCheckerCheckKeyedFieldError{
        .code = KeyedFieldCheckerCheckKeyedFieldErrorCode::ALREADY_REPORTED,
        .message = "",
    });
  }
  const Type *field = key.has_value() ? nullptr : this->keyedFieldType(type, keyType);
  if (field != nullptr) {
    return this->checkKeySetField(index, type, keyType, field, access);
  }
  auto named = this->checkObjectKeyName(type, keyType, key);
  if (named.isErr()) {
    return err(named.error());
  }

  return this->checkNamedField(index, type, named.value(), access);
}

KeyedFieldChecker::ResultKeyedFieldRead
KeyedFieldChecker::checkTupleField(IndexExpr *index, const Type *type, KeyedAccess access) {
  auto position = this->checkTupleKeyName(index, type);
  if (position.isErr()) {
    return err(position.error());
  }

  return this->checkNamedField(index, type, position.value(), access);
}

KeyedFieldChecker::ResultKeyedFieldName KeyedFieldChecker::checkTupleKeyName(IndexExpr *index,
                                                                             const Type *type) {
  index->index = this->exprChecker->checkExpr(index->index, Type::intTy());
  const Type *keyType = index->index->type;
  if (keyType->isError()) {
    return err(KeyedFieldCheckerCheckKeyedFieldError{
        .code = KeyedFieldCheckerCheckKeyedFieldErrorCode::ALREADY_REPORTED,
        .message = "",
    });
  }
  std::optional<int64_t> position =
      keyType->isInteger() ? this->constantFolder->intConstant(index->index) : std::nullopt;
  size_t count = type->fields().size();
  if (!keyType->isInteger()) {
    auto message = ErrorTextKeyedField::tupleKey(keyType->name());

    return err(KeyedFieldCheckerCheckKeyedFieldError{
        .code = KeyedFieldCheckerCheckKeyedFieldErrorCode::TUPLE_KEY_NOT_NUMBER,
        .message = message,
    });
  }
  if (!position.has_value()) {
    auto message = ErrorTextKeyedField::tupleConst();

    return err(KeyedFieldCheckerCheckKeyedFieldError{
        .code = KeyedFieldCheckerCheckKeyedFieldErrorCode::TUPLE_KEY_NOT_CONSTANT,
        .message = message,
    });
  }
  if (*position < 0 || uint64_t(*position) >= count) {
    auto message = ErrorTextKeyedField::tupleRange(type->name(), count, *position);

    return err(KeyedFieldCheckerCheckKeyedFieldError{
        .code = KeyedFieldCheckerCheckKeyedFieldErrorCode::TUPLE_KEY_OUT_OF_RANGE,
        .message = message,
    });
  }

  return ok(std::to_string(*position));
}

KeyedFieldChecker::ResultKeyedFieldName
KeyedFieldChecker::checkObjectKeyName(const Type *type, const Type *keyType,
                                      const std::optional<std::string> &key) const {
  if (!key.has_value() && keyType->nonOptional()->isNumeric()) {
    auto message = ErrorTextKeyedField::numbered(type->name());

    return err(KeyedFieldCheckerCheckKeyedFieldError{
        .code = KeyedFieldCheckerCheckKeyedFieldErrorCode::NO_NUMBERED_FIELDS,
        .message = message,
    });
  }
  if (!key.has_value() && keyType->isString()) {
    auto message = ErrorTextKeyedField::runtimeKey(type->name());

    return err(KeyedFieldCheckerCheckKeyedFieldError{
        .code = KeyedFieldCheckerCheckKeyedFieldErrorCode::KEY_NOT_CONSTANT,
        .message = message,
    });
  }
  if (!key.has_value()) {
    auto message = ErrorTextKeyedField::keyString(keyType->name());

    return err(KeyedFieldCheckerCheckKeyedFieldError{
        .code = KeyedFieldCheckerCheckKeyedFieldErrorCode::KEY_NOT_STRING,
        .message = message,
    });
  }
  if (!type->isClass() && type->field(*key) == nullptr) {
    bool isInherited = this->isInherited(*key);
    auto message = ErrorTextKeyedField::noField(*key, type->name(), isInherited);

    return err(KeyedFieldCheckerCheckKeyedFieldError{
        .code = KeyedFieldCheckerCheckKeyedFieldErrorCode::NO_SUCH_FIELD,
        .message = message,
    });
  }

  return ok(*key);
}

KeyedFieldChecker::ResultKeyedFieldRead
KeyedFieldChecker::checkKeySetField(IndexExpr *index, const Type *type, const Type *keyType,
                                    const Type *field, KeyedAccess access) {
  if (access == KeyedAccess::WRITE) {
    return this->checkKeySetWrite(index, type, keyType, field);
  }
  auto *keyed = this->keySetIndex(index, field);

  return ok(keyed);
}

Expr *KeyedFieldChecker::keySetIndex(IndexExpr *index, const Type *field) const {
  index->form = KeyedFieldIndex{};
  index->type = field;

  return index;
}

KeyedFieldChecker::ResultKeyedFieldRead KeyedFieldChecker::checkKeySetWrite(IndexExpr *index,
                                                                            const Type *type,
                                                                            const Type *keyType,
                                                                            const Type *field) {
  llvm::ArrayRef<const Type *> names =
      keyType->isUnion() ? keyType->members() : llvm::ArrayRef<const Type *>(keyType);
  bool isKnown = this->isAllAssigned(names);
  if (this->hasReadonly(type, names)) {
    auto message = ErrorTextKeyedField::readonly();

    return err(KeyedFieldCheckerCheckKeyedFieldError{
        .code = KeyedFieldCheckerCheckKeyedFieldErrorCode::READONLY_THROUGH_KEY,
        .message = message,
    });
  }
  if ((field->isOptional() || field->isUnion() || field->isLiteral()) && !isKnown) {
    auto message = ErrorTextKeyedField::byName(field->name());

    return err(KeyedFieldCheckerCheckKeyedFieldError{
        .code = KeyedFieldCheckerCheckKeyedFieldErrorCode::ASSIGNED_BY_NAME,
        .message = message,
    });
  }
  for (const Type *nameType : names) {
    auto text = ToUtf8::toUtf8(nameType->literalText());
    auto kept = this->state.getAst().keepName(text);
    this->fieldFacts->fieldsAssigned(kept);
  }
  auto *keyed = this->keySetIndex(index, field);

  return ok(keyed);
}

bool KeyedFieldChecker::isAllAssigned(llvm::ArrayRef<const Type *> names) const {
  for (const Type *nameType : names) {
    auto text = ToUtf8::toUtf8(nameType->literalText());
    if (!this->state.getFlowState().isFieldAssigned(text)) {
      return false;
    }
  }

  return true;
}

bool KeyedFieldChecker::hasReadonly(const Type *type, llvm::ArrayRef<const Type *> names) const {
  for (const Type *nameType : names) {
    auto text = ToUtf8::toUtf8(nameType->literalText());
    if (type->field(text)->readonly) {
      return true;
    }
  }

  return false;
}

KeyedFieldChecker::ResultKeyedFieldRead KeyedFieldChecker::checkNamedField(IndexExpr *index,
                                                                           const Type *type,
                                                                           const std::string &name,
                                                                           KeyedAccess access) {
  bool isAssigned = this->state.getFlowState().isFieldAssigned(name);
  bool isUnassignedWrite = access == KeyedAccess::WRITE && !isAssigned;
  if (isUnassignedWrite && type->isTuple()) {
    auto message = ErrorTextKeyedField::tupleSet();

    return err(KeyedFieldCheckerCheckKeyedFieldError{
        .code = KeyedFieldCheckerCheckKeyedFieldErrorCode::TUPLE_ELEMENT_BY_NUMBER,
        .message = message,
    });
  }
  if (isUnassignedWrite) {
    auto message = ErrorTextKeyedField::keySet();

    return err(KeyedFieldCheckerCheckKeyedFieldError{
        .code = KeyedFieldCheckerCheckKeyedFieldErrorCode::FIELD_KEY_NOT_LITERAL,
        .message = message,
    });
  }
  auto field = this->state.getAst().keepName(name);
  index->form = NamedFieldIndex{.field = field};
  auto *member = this->state.getAst().make<MemberExpr>(index->loc(), index->object, field);
  if (access == KeyedAccess::WRITE) {
    return ok(member);
  }
  auto read = this->objectChecker->checkField(member);
  if (read.isErr()) {
    return err(KeyedFieldCheckerCheckKeyedFieldError{
        .code = KeyedFieldCheckerCheckKeyedFieldErrorCode::FIELD_NOT_FOUND,
        .message = read.error().message,
    });
  }

  return ok(read.value());
}

}
