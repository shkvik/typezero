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

bool KeyedFieldChecker::wrongKey(KeyedFieldCheckerCheckKeyedFieldErrorCode code) const {
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
  key = ExprParts::withoutImplicitCasts(key);
  auto *literal = dyn_cast<StringLitExpr>(key);
  if (literal != nullptr) {
    return ToUtf8::toUtf8(literal->value);
  }
  auto *templateExpr = dyn_cast<TemplateExpr>(key);
  if (templateExpr != nullptr && templateExpr->exprs.empty()) {
    return ToUtf8::toUtf8(templateExpr->quasis[0]);
  }
  auto *name = dyn_cast<NameExpr>(key);
  if (name != nullptr && name->var() != nullptr) {
    if (std::optional<std::string> known =
            this->state.getConstantStore().findConstKey(name->var())) {
      return known;
    }
  }

  return std::nullopt;
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
  for (const Type *n : names) {
    if (!n->isStringLiteral()) {
      return nullptr;
    }
    auto text = ToUtf8::toUtf8(n->literalText());
    const auto *f = type->field(text);
    if (classDecl != nullptr) {
      const auto *field = this->classBases->findField(classDecl, text, nullptr);
      if (field == nullptr || field->isStatic() || field->access != Access::Public) {
        return nullptr;
      }
    }
    if (f == nullptr || (common != nullptr && f->type != common)) {
      return nullptr;
    }
    common = f->type;
  }

  return common;
}

KeyedFieldChecker::ResultKeyedFieldRead KeyedFieldChecker::checkKeyedField(IndexExpr *index,
                                                                           KeyedAccess access) {
  bool write = access == KeyedAccess::WRITE;
  const Type *type = index->object->type;
  index->type = Type::error();
  if (type->isTuple()) {
    auto position = this->tupleKeyName(index, type);
    if (position.isErr()) {
      return err(position.error());
    }
    return this->checkNamedField(index, type, position.value(), write);
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
    return this->checkKeySetField(index, type, keyType, field, write);
  }
  auto named = this->objectKeyName(type, keyType, key);
  if (named.isErr()) {
    return err(named.error());
  }

  return this->checkNamedField(index, type, named.value(), write);
}

KeyedFieldChecker::ResultKeyedFieldName KeyedFieldChecker::tupleKeyName(IndexExpr *index,
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
KeyedFieldChecker::objectKeyName(const Type *type, const Type *keyType,
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
                                    const Type *field, bool write) {
  if (write) {
    auto written = this->checkKeySetWrite(index, type, keyType, field);
    if (written.isErr()) {
      return written;
    }
  }
  index->form = KeyedFieldIndex{};
  index->type = field;

  return ok(index);
}

KeyedFieldChecker::ResultKeyedFieldRead KeyedFieldChecker::checkKeySetWrite(IndexExpr *index,
                                                                            const Type *type,
                                                                            const Type *keyType,
                                                                            const Type *field) {
  llvm::ArrayRef<const Type *> names =
      keyType->isUnion() ? keyType->members() : llvm::ArrayRef<const Type *>(keyType);
  bool isKnown = this->allAssigned(names);
  if (this->anyReadonly(type, names)) {
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
  for (const Type *n : names) {
    auto text = ToUtf8::toUtf8(n->literalText());
    this->fieldFacts->fieldsAssigned(this->state.getAst().keepName(text));
  }

  return ok(index);
}

bool KeyedFieldChecker::allAssigned(llvm::ArrayRef<const Type *> names) const {
  for (const Type *n : names) {
    auto text = ToUtf8::toUtf8(n->literalText());
    if (!this->state.getFlowState().isFieldAssigned(text)) {
      return false;
    }
  }

  return true;
}

bool KeyedFieldChecker::anyReadonly(const Type *type, llvm::ArrayRef<const Type *> names) const {
  for (const Type *n : names) {
    auto text = ToUtf8::toUtf8(n->literalText());
    if (type->field(text)->readonly) {
      return true;
    }
  }

  return false;
}

KeyedFieldChecker::ResultKeyedFieldRead KeyedFieldChecker::checkNamedField(IndexExpr *index,
                                                                           const Type *type,
                                                                           const std::string &name,
                                                                           bool write) {
  bool isAssigned = this->state.getFlowState().isFieldAssigned(name);
  if (write && !isAssigned && type->isTuple()) {
    auto message = ErrorTextKeyedField::tupleSet();

    return err(KeyedFieldCheckerCheckKeyedFieldError{
        .code = KeyedFieldCheckerCheckKeyedFieldErrorCode::TUPLE_ELEMENT_BY_NUMBER,
        .message = message,
    });
  }
  if (write && !isAssigned) {
    auto message = ErrorTextKeyedField::keySet();

    return err(KeyedFieldCheckerCheckKeyedFieldError{
        .code = KeyedFieldCheckerCheckKeyedFieldErrorCode::FIELD_KEY_NOT_LITERAL,
        .message = message,
    });
  }
  auto field = this->state.getAst().keepName(name);
  index->form = NamedFieldIndex{.field = field};
  auto *member = this->state.getAst().make<MemberExpr>(index->loc(), index->object, field);
  if (write) {
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
