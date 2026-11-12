#include "compiler/semantic/bodies/objects/record_literal_checker/record_literal_checker.h"
#include "compiler/semantic/bodies/expr/expr_checker/expr_checker.h"
#include "compiler/semantic/bodies/objects/dict_checker/dict_checker.h"
#include "compiler/semantic/declare/class_declarer/class_declarer.h"
#include "compiler/semantic/semantic_parts/semantic_parts.h"
#include "compiler/semantic/types/type_relations/type_relations.h"
#include "llvm/ADT/STLExtras.h"

using llvm::cast;
using llvm::dyn_cast;

namespace tz {

RecordLiteralChecker::RecordLiteralChecker(SemanticState &owner, SemanticParts &all)
    : state(owner) {
  this->dictChecker = &all.dictChecker;
  this->exprChecker = &all.exprChecker;
  this->classDeclarer = &all.classDeclarer;
  this->typeRelations = &all.typeRelations;
}

RecordLiteralChecker::ResultPlainProperty
RecordLiteralChecker::plainProperty(const ObjectLitExpr::Property &property) const {
  if (property.defaulted()) {
    auto message = ErrorTextRecordProperty::defaulted(property.name);

    return err(RecordLiteralCheckerPlainPropertyError{
        .code = RecordLiteralCheckerPlainPropertyErrorCode::DEFAULT_IN_LITERAL,
        .message = message,
    });
  }
  if (property.method()) {
    auto message = ErrorTextRecordProperty::method(property.name);

    return err(RecordLiteralCheckerPlainPropertyError{
        .code = RecordLiteralCheckerPlainPropertyErrorCode::METHOD_IN_RECORD,
        .message = message,
    });
  }

  return ok(&property);
}

void RecordLiteralChecker::reportSpread(const ObjectLitExpr::Property &property,
                                        const ResultSpreadIntoMap &spread) {
  llvm::SMLoc at = property.value->loc();
  switch (spread.error().code) {
    case RecordLiteralCheckerCheckRecordSpreadErrorCode::KEYED_REST:
      at = property.loc;
      break;
    case RecordLiteralCheckerCheckRecordSpreadErrorCode::MAY_BE_UNDEFINED:
    case RecordLiteralCheckerCheckRecordSpreadErrorCode::OTHER_VALUES:
    case RecordLiteralCheckerCheckRecordSpreadErrorCode::FIELD_TYPE_DIFFERS:
    case RecordLiteralCheckerCheckRecordSpreadErrorCode::HAS_METHODS:
    case RecordLiteralCheckerCheckRecordSpreadErrorCode::NOT_SPREADABLE:
      break;
  }
  this->state.getDiagnostics().error(at, spread.error().message);
}

Expr *RecordLiteralChecker::checkRecordLit(ObjectLitExpr *literal, const Type *record) {
  const Type *value = record->valueType();
  bool isTyped = true;
  for (ObjectLitExpr::Property &property : literal->props) {
    auto plain = this->plainProperty(property);
    if (plain.isErr()) {
      this->state.getDiagnostics().error(property.loc, plain.error().message);
      isTyped = false;
    }
    if (property.defaulted()) {
      property.value = cast<AssignExpr>(property.value)->value;
      property.form = ValueProperty{};
    }
    if (property.method()) {
      property.value = this->exprChecker->checkExpr(property.value);
      continue;
    }
    if (property.spread()) {
      property.value = this->exprChecker->checkExpr(property.value, record);
      const Type *type = property.value->type;
      if (type->isError()) {
        isTyped = false;
        continue;
      }
      auto spread = this->checkRecordSpread(property, value);
      if (spread.isErr()) {
        this->reportSpread(property, spread);
        isTyped = false;
      }
      continue;
    }
    ComputedProperty *computed = property.form.as<ComputedProperty>();
    if (computed != nullptr) {
      computed->key = this->dictChecker->checkRecordKey(computed->key);
    }
    auto *functionExpr = dyn_cast<FunctionExpr>(property.value);
    if (functionExpr != nullptr && functionExpr->fn->name.empty()) {
      functionExpr->fn->name = property.name;
    }
    std::string what = property.key() ? ErrorTextRecordProperty::valueLabel()
                                      : ErrorTextRecordProperty::keyLabel(property.name);
    property.value = this->typeRelations->coerce(
        this->exprChecker->checkExpr(property.value, value), value, what);
  }
  literal->type = isTyped ? record : Type::error();

  return literal;
}

RecordLiteralChecker::ResultSpreadIntoMap
RecordLiteralChecker::checkRecordSpread(ObjectLitExpr::Property &property, const Type *value) {
  const Type *type = property.value->type;
  if (type->isOptional() && (type->inner()->isRecord() || type->inner()->isObject())) {
    auto message = ErrorTextRecordSpread::undefined();

    return err(RecordLiteralCheckerCheckRecordSpreadError{
        .code = RecordLiteralCheckerCheckRecordSpreadErrorCode::MAY_BE_UNDEFINED,
        .message = message,
    });
  }
  if (!property.omitKeys().empty()) {
    auto message = ErrorTextRecordSpread::keyedRest();

    return err(RecordLiteralCheckerCheckRecordSpreadError{
        .code = RecordLiteralCheckerCheckRecordSpreadErrorCode::KEYED_REST,
        .message = message,
    });
  }
  if (type->isRecord() && !TypeRelations::isImplicitlyConvertible(type->valueType(), value)) {
    auto message = ErrorTextRecordSpread::values(type->name(), value->name());

    return err(RecordLiteralCheckerCheckRecordSpreadError{
        .code = RecordLiteralCheckerCheckRecordSpreadErrorCode::OTHER_VALUES,
        .message = message,
    });
  }
  if (type->isRecord()) {
    return ok(type);
  }
  bool isObject = type->isObject() && !type->isTuple() && !type->isRegExp() && !type->isMatch() &&
                  !type->isAnyObject();
  if (!isObject) {
    bool isCollection = type->isMap() || type->isSet();
    auto message = ErrorTextRecordSpread::notObject(type->name(), isCollection);

    return err(RecordLiteralCheckerCheckRecordSpreadError{
        .code = RecordLiteralCheckerCheckRecordSpreadErrorCode::NOT_SPREADABLE,
        .message = message,
    });
  }
  if (ClassDecl *classDecl = this->classDeclarer->classOf(type)) {
    this->classDeclarer->completeClass(classDecl);
  }
  for (const Field &field : type->fields()) {
    if (llvm::is_contained(property.omit(), field.name)) {
      continue;
    }
    const Type *candidate =
        field.type->isOptional() && !value->isOptional() ? field.type->inner() : field.type;
    if (!TypeRelations::isImplicitlyConvertible(candidate, value)) {
      auto message =
          ErrorTextRecordSpread::field(field.name, type->name(), field.type->name(), value->name());

      return err(RecordLiteralCheckerCheckRecordSpreadError{
          .code = RecordLiteralCheckerCheckRecordSpreadErrorCode::FIELD_TYPE_DIFFERS,
          .message = message,
      });
    }
  }
  bool hasMethods = false;
  auto isMethod = [](const ClassField &field) { return field.method(); };
  for (const ClassDecl *classDecl = this->classDeclarer->classOf(type); classDecl != nullptr;
       classDecl = classDecl->parent) {
    hasMethods |= classDecl->literal() || llvm::any_of(classDecl->fields, isMethod);
  }
  if (hasMethods) {
    auto message = ErrorTextRecordSpread::methods();

    return err(RecordLiteralCheckerCheckRecordSpreadError{
        .code = RecordLiteralCheckerCheckRecordSpreadErrorCode::HAS_METHODS,
        .message = message,
    });
  }

  return ok(type);
}

RecordLiteralChecker::ResultValueTypeJoin RecordLiteralChecker::valuesJoin(const Type *value,
                                                                           const Type *type) const {
  const Type *next = value != nullptr ? this->typeRelations->unify(value, type) : type;
  if (next == nullptr || next->isError()) {
    auto message = ErrorTextRecordValues::differ(value->name(), type->name());

    return err(RecordLiteralCheckerValuesJoinError{
        .code = RecordLiteralCheckerValuesJoinErrorCode::VALUE_TYPES_DIFFER,
        .message = message,
    });
  }

  return ok(next);
}

RecordLiteralChecker::ResultValueTypeKnow
RecordLiteralChecker::valueKnown(const Type *value) const {
  if (value->isUndefined() || value->isVoid()) {
    auto message = ErrorTextRecordValue::undefined();

    return err(RecordLiteralCheckerValueKnownError{
        .code = RecordLiteralCheckerValueKnownErrorCode::ONLY_UNDEFINED,
        .message = message,
    });
  }

  return ok(value);
}

Expr *RecordLiteralChecker::recordFromSpreads(ObjectLitExpr *literal) {
  const Type *value = nullptr;
  bool isTyped = true;
  auto meet = [&](const Type *candidate, llvm::SMLoc loc) {
    if (candidate == nullptr || candidate->isError()) {
      isTyped = false;
      return;
    }
    auto joined = this->valuesJoin(value, candidate);
    if (joined.isErr()) {
      this->state.getDiagnostics().error(loc, joined.error().message);
      isTyped = false;
      return;
    }
    value = joined.value();
  };
  for (const ObjectLitExpr::Property &property : literal->props) {
    const Type *type = property.value->type;
    if (!property.spread()) {
      meet(type, property.value->loc());
      continue;
    }
    if (type->isRecord()) {
      meet(type->valueType(), property.value->loc());
    } else if (type->isObject()) {
      for (const Field &field : type->fields()) {
        if (!llvm::is_contained(property.omit(), field.name)) {
          meet(field.type->isOptional() ? field.type->inner() : field.type, property.value->loc());
        }
      }
    }
  }
  if (!isTyped || value == nullptr) {
    literal->type = Type::error();
    return literal;
  }
  auto known = this->valueKnown(value);
  if (known.isErr()) {
    this->state.getDiagnostics().error(literal->loc(), known.error().message);
    literal->type = Type::error();
    return literal;
  }
  const auto *record = Type::dict(Type::DictKind::Record, Type::string(), value);
  for (ObjectLitExpr::Property &property : literal->props) {
    if (!property.spread()) {
      property.value = this->typeRelations->convert(property.value, value);
      continue;
    }
    auto spread = this->checkRecordSpread(property, value);
    if (spread.isErr()) {
      this->reportSpread(property, spread);
      isTyped = false;
    }
  }
  literal->type = isTyped ? record : Type::error();

  return literal;
}

}
