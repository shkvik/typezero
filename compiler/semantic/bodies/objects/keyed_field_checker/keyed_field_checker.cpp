#include "compiler/semantic/bodies/objects/keyed_field_checker/keyed_field_checker.h"
#include "compiler/program/library/object/object_member_table/object_member_table.h"
#include "compiler/program/to_utf8/to_utf8.h"
#include "compiler/semantic/bodies/classes/class_bases/class_bases.h"
#include "compiler/semantic/bodies/expr/expr_checker/expr_checker.h"
#include "compiler/semantic/bodies/objects/object_checker/object_checker.h"
#include "compiler/semantic/declare/class_declarer/class_declarer.h"
#include "compiler/semantic/declare/constant_folder/constant_folder.h"
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
  this->fieldFacts = &all.getFieldFacts();
  this->exprChecker = &all.getExprChecker();
  this->literalTypes = &all.getLiteralTypes();
  this->objectChecker = &all.getObjectChecker();
  this->typeRelations = &all.getTypeRelations();
  this->constantFolder = &all.getConstantFolder();
}

bool KeyedFieldChecker::isInherited(llvm::StringRef name) {
  std::optional<ObjectMember> member = ObjectMemberTable::find(name);

  return member.has_value();
}

bool KeyedFieldChecker::wrongKey(KeyedFieldCheckerCheckKeyedFieldErrorCode code) {
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
  while (auto *conv = dyn_cast<CastExpr>(key)) {
    if (!conv->isImplicit()) {
      break;
    }
    key = conv->operand;
  }
  if (auto *lit = dyn_cast<StringLitExpr>(key)) {
    return ToUtf8::toUtf8(lit->value);
  }
  if (auto *tmpl = dyn_cast<TemplateExpr>(key); tmpl && tmpl->exprs.empty()) {
    return ToUtf8::toUtf8(tmpl->quasis[0]);
  }
  if (auto *name = dyn_cast<NameExpr>(key); name && name->var()) {
    if (std::optional<std::string> known =
            this->state.getConstantStore().findConstKey(name->var())) {
      return known;
    }
  }

  return std::nullopt;
}

void KeyedFieldChecker::keyNames(const Expr *key,
                                 llvm::function_ref<void(llvm::StringRef)> each) const {
  if (auto *lit = dyn_cast<StringLitExpr>(key)) {
    each(ToUtf8::toUtf8(lit->value));
  } else if (auto *tmpl = dyn_cast<TemplateExpr>(key); tmpl && tmpl->exprs.empty()) {
    each(ToUtf8::toUtf8(tmpl->quasis[0]));
  } else if (auto *number = dyn_cast<IntLitExpr>(key)) {
    each(std::to_string(number->value));
  } else if (auto *name = dyn_cast<NameExpr>(key)) {
    for (const std::string &text : this->state.getInventoryStore().getStringConstants(name->name)) {
      each(text);
    }
  } else if (auto *member = dyn_cast<MemberExpr>(key)) {
    if (auto *object = dyn_cast<NameExpr>(member->object)) {
      for (const std::string &text : this->state.getInventoryStore().getStringConstants(
               (object->name + "." + member->name).str())) {
        each(text);
      }
    }
  }
}

const Type *KeyedFieldChecker::keyedFieldType(const Type *type, const Type *keyType) {
  if (type->isTuple() || !type->isObject()) {
    return nullptr;
  }
  const ClassDecl *cls = type->isClass() ? ClassDeclarer::classOf(type) : nullptr;
  if (type->isClass() && !cls) {
    return nullptr;
  }
  llvm::ArrayRef<const Type *> names =
      keyType->isUnion() ? keyType->members() : llvm::ArrayRef<const Type *>(keyType);
  const Type *common = nullptr;
  for (const Type *n : names) {
    if (!n->isStringLiteral()) {
      return nullptr;
    }
    std::string text = ToUtf8::toUtf8(n->literalText());
    const Field *f = type->field(text);
    if (cls) {
      const ClassField *field = ClassBases::findField(cls, text, nullptr);
      if (!field || field->isStatic() || field->access != Access::Public) {
        return nullptr;
      }
    }
    if (!f || (common && f->type != common)) {
      return nullptr;
    }
    common = f->type;
  }

  return common;
}

KeyedFieldChecker::ResultKeyedFieldRead KeyedFieldChecker::checkKeyedField(IndexExpr *index,
                                                                           bool write) {
  const Type *type = index->object->type;
  index->type = Type::error();
  std::string name;
  if (type->isTuple()) {
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
      std::string message = ErrorTextKeyedField::tupleKey(keyType->name());

      return err(KeyedFieldCheckerCheckKeyedFieldError{
          .code = KeyedFieldCheckerCheckKeyedFieldErrorCode::TUPLE_KEY_NOT_NUMBER,
          .message = message,
      });
    }
    if (!position) {
      std::string message = ErrorTextKeyedField::tupleConst();

      return err(KeyedFieldCheckerCheckKeyedFieldError{
          .code = KeyedFieldCheckerCheckKeyedFieldErrorCode::TUPLE_KEY_NOT_CONSTANT,
          .message = message,
      });
    }
    if (*position < 0 || uint64_t(*position) >= count) {
      std::string message = ErrorTextKeyedField::tupleRange(type->name(), count, *position);

      return err(KeyedFieldCheckerCheckKeyedFieldError{
          .code = KeyedFieldCheckerCheckKeyedFieldErrorCode::TUPLE_KEY_OUT_OF_RANGE,
          .message = message,
      });
    }
    name = std::to_string(*position);
  } else {
    index->index = this->exprChecker->checkExpr(index->index);
    std::optional<std::string> key = this->constantKey(index->index);
    if (const Type *texts =
            key ? nullptr : this->literalTypes->constTextType(index->index, nullptr);
        texts && KeyedFieldChecker::keyedFieldType(type, texts)) {
      index->index = this->typeRelations->convert(index->index, texts);
    }
    const Type *keyType = index->index->type;
    if (keyType->isError()) {
      return err(KeyedFieldCheckerCheckKeyedFieldError{
          .code = KeyedFieldCheckerCheckKeyedFieldErrorCode::ALREADY_REPORTED,
          .message = "",
      });
    }
    if (const Type *field = key ? nullptr : KeyedFieldChecker::keyedFieldType(type, keyType)) {
      llvm::ArrayRef<const Type *> names =
          keyType->isUnion() ? keyType->members() : llvm::ArrayRef<const Type *>(keyType);
      if (write) {
        auto assigned = [&](const Type *n) {
          return this->state.getFlowState().isFieldAssigned(ToUtf8::toUtf8(n->literalText()));
        };
        auto readonly = [&](const Type *n) {
          return type->field(ToUtf8::toUtf8(n->literalText()))->readonly;
        };
        bool known = llvm::all_of(names, assigned);
        if (llvm::any_of(names, readonly)) {
          std::string message = ErrorTextKeyedField::readonly();

          return err(KeyedFieldCheckerCheckKeyedFieldError{
              .code = KeyedFieldCheckerCheckKeyedFieldErrorCode::READONLY_THROUGH_KEY,
              .message = message,
          });
        }
        if ((field->isOptional() || field->isUnion() || field->isLiteral()) && !known) {
          std::string message = ErrorTextKeyedField::byName(field->name());

          return err(KeyedFieldCheckerCheckKeyedFieldError{
              .code = KeyedFieldCheckerCheckKeyedFieldErrorCode::ASSIGNED_BY_NAME,
              .message = message,
          });
        }
        for (const Type *n : names) {
          this->fieldFacts->fieldsAssigned(
              this->state.getAst().keepName(ToUtf8::toUtf8(n->literalText())));
        }
      }
      index->form = KeyedFieldIndex{};
      index->type = field;
      return ok(index);
    }
    if (!key && keyType->nonOptional()->isNumeric()) {
      std::string message = ErrorTextKeyedField::numbered(type->name());

      return err(KeyedFieldCheckerCheckKeyedFieldError{
          .code = KeyedFieldCheckerCheckKeyedFieldErrorCode::NO_NUMBERED_FIELDS,
          .message = message,
      });
    }
    if (!key && keyType->isString()) {
      std::string message = ErrorTextKeyedField::runtimeKey(type->name());

      return err(KeyedFieldCheckerCheckKeyedFieldError{
          .code = KeyedFieldCheckerCheckKeyedFieldErrorCode::KEY_NOT_CONSTANT,
          .message = message,
      });
    }
    if (!key) {
      std::string message = ErrorTextKeyedField::keyString(keyType->name());

      return err(KeyedFieldCheckerCheckKeyedFieldError{
          .code = KeyedFieldCheckerCheckKeyedFieldErrorCode::KEY_NOT_STRING,
          .message = message,
      });
    }
    if (!type->isClass() && !type->field(*key)) {
      bool inherited = KeyedFieldChecker::isInherited(*key);
      std::string message = ErrorTextKeyedField::noField(*key, type->name(), inherited);

      return err(KeyedFieldCheckerCheckKeyedFieldError{
          .code = KeyedFieldCheckerCheckKeyedFieldErrorCode::NO_SUCH_FIELD,
          .message = message,
      });
    }
    name = std::move(*key);
  }
  if (write && !this->state.getFlowState().isFieldAssigned(name) && type->isTuple()) {
    std::string message = ErrorTextKeyedField::tupleSet();

    return err(KeyedFieldCheckerCheckKeyedFieldError{
        .code = KeyedFieldCheckerCheckKeyedFieldErrorCode::TUPLE_ELEMENT_BY_NUMBER,
        .message = message,
    });
  }
  if (write && !this->state.getFlowState().isFieldAssigned(name)) {
    std::string message = ErrorTextKeyedField::keySet();

    return err(KeyedFieldCheckerCheckKeyedFieldError{
        .code = KeyedFieldCheckerCheckKeyedFieldErrorCode::FIELD_KEY_NOT_LITERAL,
        .message = message,
    });
  }
  llvm::StringRef field = this->state.getAst().keepName(name);
  index->form = NamedFieldIndex{.field = field};
  auto *member = this->state.getAst().make<MemberExpr>(index->loc(), index->object, field);
  if (write) {
    return ok(member);
  }

  ObjectChecker::ResultObjectFieldValue read = this->objectChecker->checkField(member);
  if (read.isErr()) {
    return err(KeyedFieldCheckerCheckKeyedFieldError{
        .code = KeyedFieldCheckerCheckKeyedFieldErrorCode::FIELD_NOT_FOUND,
        .message = read.error().message,
    });
  }

  return ok(read.value());
}

}
