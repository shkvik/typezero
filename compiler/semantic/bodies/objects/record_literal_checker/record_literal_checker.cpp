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
RecordLiteralChecker::checkPlainProperty(const ObjectLitExpr::Property &property) const {
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
                                        const RecordLiteralCheckerCheckRecordSpreadError &error) {
  auto at = this->spreadErrorPlace(property, error.code);
  this->state.getDiagnostics().error(at, error.message);
}

llvm::SMLoc
RecordLiteralChecker::spreadErrorPlace(const ObjectLitExpr::Property &property,
                                       RecordLiteralCheckerCheckRecordSpreadErrorCode code) const {
  switch (code) {
    case RecordLiteralCheckerCheckRecordSpreadErrorCode::KEYED_REST:
      return property.loc;
    case RecordLiteralCheckerCheckRecordSpreadErrorCode::MAY_BE_UNDEFINED:
    case RecordLiteralCheckerCheckRecordSpreadErrorCode::OTHER_VALUES:
    case RecordLiteralCheckerCheckRecordSpreadErrorCode::FIELD_TYPE_DIFFERS:
    case RecordLiteralCheckerCheckRecordSpreadErrorCode::HAS_METHODS:
    case RecordLiteralCheckerCheckRecordSpreadErrorCode::NOT_SPREADABLE:
      return property.value->loc();
  }

  return property.value->loc();
}

Expr *RecordLiteralChecker::checkRecordLit(ObjectLitExpr *literal, const Type *record) {
  bool hasFailed = false;
  for (ObjectLitExpr::Property &property : literal->props) {
    bool hasPropertyFailed = this->reportRecordProperty(property, record);
    hasFailed = hasFailed || hasPropertyFailed;
  }
  literal->type = hasFailed ? Type::error() : record;

  return literal;
}

bool RecordLiteralChecker::reportRecordProperty(ObjectLitExpr::Property &property,
                                                const Type *record) {
  auto plain = this->checkPlainProperty(property);
  if (plain.isErr()) {
    this->state.getDiagnostics().error(property.loc, plain.error().message);
  }
  bool isPlainFailed = plain.isErr();
  if (property.defaulted()) {
    property.value = cast<AssignExpr>(property.value)->value;
    property.form = ValueProperty{};
  }
  if (property.method()) {
    property.value = this->exprChecker->checkExpr(property.value);
    return isPlainFailed;
  }
  if (property.spread()) {
    bool isSpreadFailed = this->reportRecordSpread(property, record);
    return isPlainFailed || isSpreadFailed;
  }
  this->walkRecordValue(property, record->valueType());

  return isPlainFailed;
}

bool RecordLiteralChecker::reportRecordSpread(ObjectLitExpr::Property &property,
                                              const Type *record) {
  property.value = this->exprChecker->checkExpr(property.value, record);
  if (property.value->type->isError()) {
    return true;
  }

  return this->reportSpreadIntoRecord(property, record->valueType());
}

bool RecordLiteralChecker::reportSpreadIntoRecord(ObjectLitExpr::Property &property,
                                                  const Type *value) {
  auto spread = this->checkRecordSpread(property, value);
  if (spread.isOk()) {
    return false;
  }
  this->reportSpread(property, spread.error());

  return true;
}

void RecordLiteralChecker::walkRecordValue(ObjectLitExpr::Property &property, const Type *value) {
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
  auto *checked = this->exprChecker->checkExpr(property.value, value);
  property.value = this->typeRelations->coerce(checked, value, what);
}

RecordLiteralChecker::ResultSpreadIntoMap
RecordLiteralChecker::checkRecordSpread(ObjectLitExpr::Property &property, const Type *value) {
  const Type *type = property.value->type;
  auto source = this->checkSpreadSource(property);
  if (source.isErr()) {
    return err(source.error());
  }
  if (type->isRecord()) {
    return this->checkRecordValues(type, value);
  }

  return this->checkObjectSpread(property, value);
}

RecordLiteralChecker::ResultSpreadIntoMap
RecordLiteralChecker::checkSpreadSource(const ObjectLitExpr::Property &property) const {
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

  return ok(type);
}

RecordLiteralChecker::ResultSpreadIntoMap
RecordLiteralChecker::checkRecordValues(const Type *type, const Type *value) const {
  if (!TypeRelations::isImplicitlyConvertible(type->valueType(), value)) {
    auto message = ErrorTextRecordSpread::values(type->name(), value->name());

    return err(RecordLiteralCheckerCheckRecordSpreadError{
        .code = RecordLiteralCheckerCheckRecordSpreadErrorCode::OTHER_VALUES,
        .message = message,
    });
  }

  return ok(type);
}

RecordLiteralChecker::ResultSpreadIntoMap
RecordLiteralChecker::checkObjectSpread(const ObjectLitExpr::Property &property,
                                        const Type *value) {
  const Type *type = property.value->type;
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
  auto *classDecl = this->classDeclarer->classOf(type);
  if (classDecl != nullptr) {
    this->classDeclarer->completeClass(classDecl);
  }
  auto fields = this->checkSpreadFields(property, value);
  if (fields.isErr()) {
    return err(fields.error());
  }
  if (this->hasClassMethods(type)) {
    auto message = ErrorTextRecordSpread::methods();

    return err(RecordLiteralCheckerCheckRecordSpreadError{
        .code = RecordLiteralCheckerCheckRecordSpreadErrorCode::HAS_METHODS,
        .message = message,
    });
  }

  return ok(type);
}

RecordLiteralChecker::ResultSpreadIntoMap
RecordLiteralChecker::checkSpreadFields(const ObjectLitExpr::Property &property,
                                        const Type *value) const {
  const Type *type = property.value->type;
  for (const Field &field : type->fields()) {
    bool isKept = !llvm::is_contained(property.omit(), field.name);
    const Type *candidate =
        field.type->isOptional() && !value->isOptional() ? field.type->inner() : field.type;
    if (isKept && !TypeRelations::isImplicitlyConvertible(candidate, value)) {
      auto message =
          ErrorTextRecordSpread::field(field.name, type->name(), field.type->name(), value->name());

      return err(RecordLiteralCheckerCheckRecordSpreadError{
          .code = RecordLiteralCheckerCheckRecordSpreadErrorCode::FIELD_TYPE_DIFFERS,
          .message = message,
      });
    }
  }

  return ok(type);
}

bool RecordLiteralChecker::hasClassMethods(const Type *type) const {
  const ClassDecl *classDecl = this->classDeclarer->classOf(type);
  for (; classDecl != nullptr; classDecl = classDecl->parent) {
    bool hasMethodField = llvm::any_of(classDecl->fields,
                                       std::bind_front(&RecordLiteralChecker::isMethodField, this));
    if (classDecl->literal() != nullptr || hasMethodField) {
      return true;
    }
  }

  return false;
}

bool RecordLiteralChecker::isMethodField(const ClassField &field) const {
  return field.method() != nullptr;
}

RecordLiteralChecker::ResultValueTypeJoin
RecordLiteralChecker::checkValuesJoin(const Type *value, const Type *type) const {
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
RecordLiteralChecker::checkValueKnown(const Type *value) const {
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
  auto candidates = this->valueCandidates(literal);
  auto joined = RecordValue{
      .value = nullptr,
      .isTyped = true,
  };
  for (const ValueCandidate &candidate : candidates) {
    joined = this->joinValue(joined, candidate);
  }
  if (!joined.isTyped || joined.value == nullptr) {
    literal->type = Type::error();
    return literal;
  }
  const Type *value = joined.value;
  auto known = this->checkValueKnown(value);
  if (known.isErr()) {
    this->state.getDiagnostics().error(literal->loc(), known.error().message);
    literal->type = Type::error();
    return literal;
  }
  const auto *record = Type::dict(Type::DictKind::Record, Type::string(), value);
  bool hasFailed = false;
  for (ObjectLitExpr::Property &property : literal->props) {
    if (property.spread()) {
      bool hasSpreadFailed = this->reportSpreadIntoRecord(property, value);
      hasFailed = hasFailed || hasSpreadFailed;
    } else {
      property.value = this->typeRelations->convert(property.value, value);
    }
  }
  literal->type = hasFailed ? Type::error() : record;

  return literal;
}

std::vector<RecordLiteralChecker::ValueCandidate>
RecordLiteralChecker::valueCandidates(const ObjectLitExpr *literal) const {
  auto candidates = std::vector<ValueCandidate>();
  for (const ObjectLitExpr::Property &property : literal->props) {
    const Type *type = property.value->type;
    llvm::SMLoc at = property.value->loc();
    if (!property.spread()) {
      candidates.push_back(ValueCandidate{.type = type, .loc = at});
    } else if (type->isRecord()) {
      candidates.push_back(ValueCandidate{.type = type->valueType(), .loc = at});
    } else if (type->isObject()) {
      this->addFieldCandidates(property, candidates);
    }
  }

  return candidates;
}

void RecordLiteralChecker::addFieldCandidates(const ObjectLitExpr::Property &property,
                                              std::vector<ValueCandidate> &candidates) const {
  for (const Field &field : property.value->type->fields()) {
    if (!llvm::is_contained(property.omit(), field.name)) {
      const Type *type = field.type->isOptional() ? field.type->inner() : field.type;
      candidates.push_back(ValueCandidate{.type = type, .loc = property.value->loc()});
    }
  }
}

RecordLiteralChecker::RecordValue RecordLiteralChecker::joinValue(const RecordValue &current,
                                                                  const ValueCandidate &candidate) {
  if (candidate.type == nullptr || candidate.type->isError()) {
    return RecordValue{
        .value = current.value,
        .isTyped = false,
    };
  }
  auto joined = this->checkValuesJoin(current.value, candidate.type);
  if (joined.isErr()) {
    this->state.getDiagnostics().error(candidate.loc, joined.error().message);
    return RecordValue{
        .value = current.value,
        .isTyped = false,
    };
  }

  return RecordValue{
      .value = joined.value(),
      .isTyped = current.isTyped,
  };
}

}
