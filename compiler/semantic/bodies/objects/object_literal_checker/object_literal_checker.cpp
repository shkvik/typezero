#include "compiler/semantic/bodies/objects/object_literal_checker/object_literal_checker.h"
#include "compiler/program/library/builtin_names/prelude_type_table/prelude_type_table.h"
#include "compiler/program/to_utf8/to_utf8.h"
#include "compiler/semantic/bodies/classes/method_literal_checker/method_literal_checker.h"
#include "compiler/semantic/bodies/expr/expr_checker/expr_checker.h"
#include "compiler/semantic/bodies/objects/keyed_field_checker/keyed_field_checker.h"
#include "compiler/semantic/bodies/objects/record_literal_checker/record_literal_checker.h"
#include "compiler/semantic/declare/class_declarer/class_declarer.h"
#include "compiler/semantic/library/json/json_checker/json_checker.h"
#include "compiler/semantic/semantic_parts/semantic_parts.h"
#include "compiler/semantic/types/literal_types/literal_types.h"
#include "compiler/semantic/types/type_relations/type_relations.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/ADT/StringExtras.h"
#include "llvm/Support/ConvertUTF.h"

using llvm::cast;
using llvm::dyn_cast;

namespace tz {

ObjectLiteralChecker::ObjectLiteralChecker(SemanticState &owner, SemanticParts &all)
    : state(owner) {
  this->exprChecker = &all.exprChecker;
  this->jsonChecker = &all.jsonChecker;
  this->literalTypes = &all.literalTypes;
  this->classDeclarer = &all.classDeclarer;
  this->typeRelations = &all.typeRelations;
  this->keyedFieldChecker = &all.keyedFieldChecker;
  this->methodLiteralChecker = &all.methodLiteralChecker;
  this->recordLiteralChecker = &all.recordLiteralChecker;
}

ObjectLiteralChecker::ResultPropertyHasValue
ObjectLiteralChecker::checkPropertyPlain(const ObjectLitExpr::Property &property) const {
  if (property.defaulted()) {
    auto message = ErrorTextObjectLiteral::defaulted(property.name);

    return err(ObjectLiteralCheckerPropertyPlainError{
        .code = ObjectLiteralCheckerPropertyPlainErrorCode::DEFAULT_VALUE,
        .message = message,
    });
  }

  return ok(&property);
}

ObjectLiteralChecker::ResultComputedKeyKnown
ObjectLiteralChecker::checkComputedKey(const Type *keyType) const {
  if (keyType->nonOptional()->isNumeric()) {
    auto message = ErrorTextObjectLiteral::numberKey();

    return err(ObjectLiteralCheckerComputedKeyError{
        .code = ObjectLiteralCheckerComputedKeyErrorCode::NUMBER_KEY,
        .message = message,
    });
  }
  if (keyType->isString()) {
    auto message = ErrorTextObjectLiteral::runtimeKey();

    return err(ObjectLiteralCheckerComputedKeyError{
        .code = ObjectLiteralCheckerComputedKeyErrorCode::RUNTIME_KEY,
        .message = message,
    });
  }
  auto message = ErrorTextObjectLiteral::keyType(keyType->name());

  return err(ObjectLiteralCheckerComputedKeyError{
      .code = ObjectLiteralCheckerComputedKeyErrorCode::NOT_A_STRING,
      .message = message,
  });
}

ObjectLiteralChecker::ResultValueTypeIsKnown
ObjectLiteralChecker::checkInferredValue(const Type *value) const {
  if (value->isVoid() || value->isUndefined()) {
    auto message = ErrorTextObjectLiteral::inferFrom(value->name());

    return err(ObjectLiteralCheckerInferredValueError{
        .code = ObjectLiteralCheckerInferredValueErrorCode::CANNOT_INFER,
        .message = message,
    });
  }

  return ok(value);
}

ObjectLiteralChecker::ResultKeyedValuesAgree
ObjectLiteralChecker::checkSameKeyedType(llvm::StringRef name, const Field &same,
                                         const Type *value) const {
  if (same.type != Type::optional(value)) {
    auto message = ErrorTextObjectLiteral::keyTypes(name);

    return err(ObjectLiteralCheckerSameKeyedTypeError{
        .code = ObjectLiteralCheckerSameKeyedTypeErrorCode::FIELD_TYPES_DIFFER,
        .message = message,
    });
  }

  return ok(value);
}

ObjectLiteralChecker::ResultKeyedObjectTyped
ObjectLiteralChecker::checkKeyedTarget(const Type *target, const Type *keyType) const {
  if (target == nullptr) {
    auto message = ErrorTextObjectLiteral::keyNoType(keyType->name());

    return err(ObjectLiteralCheckerKeyedTargetError{
        .code = ObjectLiteralCheckerKeyedTargetErrorCode::NO_OBJECT_TYPE,
        .message = message,
    });
  }

  return ok(target);
}

ObjectLiteralChecker::ResultKeyedFieldsAgree
ObjectLiteralChecker::checkKeyedField(const Type *target, const Type *keyType) const {
  const auto *field = this->keyedFieldChecker->keyedFieldType(target, keyType);
  if (field == nullptr) {
    auto message = ErrorTextObjectLiteral::keyFields(keyType->name(), target->name());

    return err(ObjectLiteralCheckerKeyedFieldError{
        .code = ObjectLiteralCheckerKeyedFieldErrorCode::FIELDS_DIFFER,
        .message = message,
    });
  }
  if (!field->isOptional()) {
    auto message = ErrorTextObjectLiteral::keyedOpt(keyType->name(), field->name());

    return err(ObjectLiteralCheckerKeyedFieldError{
        .code = ObjectLiteralCheckerKeyedFieldErrorCode::NOT_OPTIONAL,
        .message = message,
    });
  }

  return ok(field);
}

ObjectLiteralChecker::ResultFieldNameIsFresh
ObjectLiteralChecker::checkUniqueName(const ObjectLitExpr *literal, size_t index,
                                      llvm::ArrayRef<bool> named) const {
  if (!named[index]) {
    return ok(&literal->props[index]);
  }
  for (size_t earlier = 0; earlier < index; ++earlier) {
    if (named[earlier] && literal->props[earlier].name == literal->props[index].name) {
      auto message = ErrorTextObjectLiteral::duplicate(literal->props[index].name);

      return err(ObjectLiteralCheckerUniqueNameError{
          .code = ObjectLiteralCheckerUniqueNameErrorCode::DUPLICATE_FIELD,
          .message = message,
      });
    }
  }

  return ok(&literal->props[index]);
}

ObjectLiteralChecker::ResultKeyWithoutSpread
ObjectLiteralChecker::checkKeyedPlacement(const ObjectLitExpr *literal, const Type *keyType) const {
  bool hasMethods = this->hasMethod(literal);
  if (literal->hasSpread() || hasMethods) {
    auto message = ErrorTextObjectLiteral::keySpread(keyType->name());

    return err(ObjectLiteralCheckerKeyedPlacementError{
        .code = ObjectLiteralCheckerKeyedPlacementErrorCode::KEY_WITH_SPREAD,
        .message = message,
    });
  }

  return ok(keyType);
}

ObjectLiteralChecker::ResultTargetFieldFound
ObjectLiteralChecker::checkTargetField(const Type *target, llvm::StringRef name) const {
  const auto *field = target->field(name);
  if (field == nullptr && target->overload(name) != nullptr) {
    return err(ObjectLiteralCheckerTargetFieldError{
        .code = ObjectLiteralCheckerTargetFieldErrorCode::OVERLOAD_NAME,
        .message = "",
    });
  }
  if (field == nullptr) {
    auto streamError = PreludeTypeTable::getName(PreludeType::STREAM_ERROR);
    bool isStream = target->name() == streamError;
    auto message = ErrorTextObjectLiteral::noField(target->name(), name, isStream);

    return err(ObjectLiteralCheckerTargetFieldError{
        .code = ObjectLiteralCheckerTargetFieldErrorCode::NO_SUCH_FIELD,
        .message = message,
    });
  }

  return ok(field);
}

ObjectLiteralChecker::ResultRequiredFieldSet
ObjectLiteralChecker::checkFieldGiven(const ObjectLitExpr *literal, const Field &field,
                                      const Type *target) const {
  bool isGiven = this->hasPropertyNamed(literal, field.name);
  if (!isGiven && !field.type->isOptional()) {
    auto message = ErrorTextObjectLiteral::missing(field.name, target->name());

    return err(ObjectLiteralCheckerFieldGivenError{
        .code = ObjectLiteralCheckerFieldGivenErrorCode::MISSING_FIELD,
        .message = message,
    });
  }

  return ok(&field);
}

bool ObjectLiteralChecker::hasPropertyNamed(const ObjectLitExpr *literal,
                                            llvm::StringRef name) const {
  for (const ObjectLitExpr::Property &property : literal->props) {
    if (property.name == name) {
      return true;
    }
  }

  return false;
}

ObjectLiteralChecker::ResultUsableFieldValue
ObjectLiteralChecker::checkValueTyped(const Type *type, llvm::StringRef what,
                                      llvm::StringRef name) const {
  if (type->isVoid()) {
    auto message = ErrorTextObjectLiteral::voidValue(what);

    return err(ObjectLiteralCheckerValueTypedError{
        .code = ObjectLiteralCheckerValueTypedErrorCode::VOID_VALUE,
        .message = message,
    });
  }
  if (type->isUndefined()) {
    auto message = ErrorTextObjectLiteral::undefOf(what, name);

    return err(ObjectLiteralCheckerValueTypedError{
        .code = ObjectLiteralCheckerValueTypedErrorCode::UNDEFINED_VALUE,
        .message = message,
    });
  }

  return ok(type);
}

Expr *ObjectLiteralChecker::checkObjectLit(ObjectLitExpr *literal, const Type *expected) {
  const Type *wanted = expected != nullptr ? expected->nonOptional() : nullptr;
  if (wanted != nullptr && wanted->isJson()) {
    return this->jsonChecker->checkJsonObjectLit(literal);
  }
  if (wanted != nullptr && wanted->isRecord()) {
    return this->recordLiteralChecker->checkRecordLit(literal, wanted);
  }
  const Type *expectedUnion = wanted != nullptr && wanted->isUnion() ? wanted : nullptr;
  auto shape = ObjectLitShape{
      .target = this->isUsableTarget(wanted) ? wanted : nullptr,
      .named = llvm::SmallVector<bool, 8>(literal->props.size(), true),
      .isTyped = true,
      .isInferred = false,
  };
  for (size_t index = 0; index < literal->props.size(); ++index) {
    this->walkPropertyKey(literal, index, shape);
  }
  this->walkUniqueNames(literal, shape);
  Expr *record =
      expectedUnion != nullptr ? this->pickUnionTarget(literal, expectedUnion, shape) : nullptr;
  if (record != nullptr) {
    return record;
  }
  if (shape.target != nullptr) {
    this->walkThisMethods(literal, shape.target);
  }
  bool isKeyedOnly =
      shape.target == nullptr && !literal->props.empty() && this->isAllKeyed(literal);
  Expr *inferFailed = isKeyedOnly ? this->checkInferredKeys(literal, shape) : nullptr;
  if (inferFailed != nullptr) {
    return inferFailed;
  }
  auto *keyedFailed = this->checkKeyedProperties(literal, shape);
  if (keyedFailed != nullptr) {
    return keyedFailed;
  }
  if (this->hasMethod(literal)) {
    return this->methodLiteralChecker->checkMethodLiteral(literal, shape.target);
  }
  if (literal->hasSpread()) {
    return this->checkSpreadLit(literal, shape.target, shape.named, shape.isTyped);
  }
  auto fields = this->checkNamedProperties(literal, shape);

  return this->finishObjectLit(literal, shape, std::move(fields));
}

bool ObjectLiteralChecker::isUsableTarget(const Type *type) const {
  return type != nullptr && type->isObject() && !type->isIncomplete() && !type->isTuple() &&
         !type->isClass() && !type->isRegExp() && !type->isMatch() && !type->isDate() &&
         !type->isIntl() && !type->isBytes() && !type->isCrypto() && !type->isAnyObject();
}

void ObjectLiteralChecker::walkPropertyKey(ObjectLitExpr *literal, size_t index,
                                           ObjectLitShape &shape) {
  ObjectLitExpr::Property &property = literal->props[index];
  shape.named[index] = !property.spread();
  auto plain = this->checkPropertyPlain(property);
  if (plain.isErr()) {
    this->state.getDiagnostics().error(property.loc, plain.error().message);
    property.value = cast<AssignExpr>(property.value)->value;
    property.form = ValueProperty{};
    shape.isTyped = false;
  }
  ComputedProperty *computed = property.form.as<ComputedProperty>();
  if (computed != nullptr) {
    this->walkComputedKey(property, *computed, index, shape);
  }
}

void ObjectLiteralChecker::walkComputedKey(ObjectLitExpr::Property &property,
                                           ComputedProperty &computed, size_t index,
                                           ObjectLitShape &shape) {
  computed.key = this->exprChecker->checkExpr(computed.key);
  const Type *keyType = property.key()->type;
  auto name = this->keyedFieldChecker->constantKey(property.key());
  if (name.has_value()) {
    property.name = this->state.getAst().keepName(std::move(*name));
    this->state.getInventoryStore().addFieldName(property.name);
    return;
  }
  shape.named[index] = false;
  if (this->isLiteralKeys(keyType)) {
    computed.keyed = true;
    return;
  }
  shape.isTyped = false;
  if (keyType->isError()) {
    return;
  }
  auto key = this->checkComputedKey(keyType);
  if (key.isErr()) {
    this->state.getDiagnostics().error(property.key()->loc(), key.error().message);
  }
}

bool ObjectLiteralChecker::isLiteralKeys(const Type *keyType) const {
  llvm::ArrayRef<const Type *> names =
      keyType->isUnion() ? keyType->members() : llvm::ArrayRef<const Type *>(keyType);
  for (const Type *nameType : names) {
    if (!nameType->isStringLiteral()) {
      return false;
    }
  }

  return true;
}

void ObjectLiteralChecker::walkUniqueNames(ObjectLitExpr *literal, ObjectLitShape &shape) {
  for (size_t index = 0; index < literal->props.size(); ++index) {
    auto fresh = this->checkUniqueName(literal, index, shape.named);
    if (fresh.isErr()) {
      this->state.getDiagnostics().error(literal->props[index].loc, fresh.error().message);
      shape.isTyped = false;
    }
  }
}

Expr *ObjectLiteralChecker::pickUnionTarget(ObjectLitExpr *literal, const Type *expectedUnion,
                                            ObjectLitShape &shape) {
  const auto *member = this->literalTypes->literalMember(literal, expectedUnion);
  shape.target = this->isUsableTarget(member) ? member : nullptr;
  if (shape.target != nullptr) {
    return nullptr;
  }
  for (const Type *memberType : expectedUnion->members()) {
    if (memberType->isRecord()) {
      return this->recordLiteralChecker->checkRecordLit(literal, memberType);
    }
  }

  return nullptr;
}

void ObjectLiteralChecker::walkThisMethods(ObjectLitExpr *literal, const Type *target) const {
  for (ObjectLitExpr::Property &property : literal->props) {
    const Field *wanted = property.method() ? target->field(property.name) : nullptr;
    bool isBound = wanted != nullptr && wanted->type->nonOptional()->isFunction() &&
                   wanted->type->nonOptional()->hasThis();
    if (isBound) {
      property.form = ValueProperty{};
    }
  }
}

bool ObjectLiteralChecker::isAllKeyed(const ObjectLitExpr *literal) const {
  for (const ObjectLitExpr::Property &property : literal->props) {
    if (!property.keyed()) {
      return false;
    }
  }

  return true;
}

bool ObjectLiteralChecker::hasMethod(const ObjectLitExpr *literal) const {
  for (const ObjectLitExpr::Property &property : literal->props) {
    if (property.method()) {
      return true;
    }
  }

  return false;
}

const Field *ObjectLiteralChecker::fieldNamed(const std::vector<Field> &fields,
                                              llvm::StringRef name) const {
  for (const Field &field : fields) {
    if (field.name == name) {
      return &field;
    }
  }

  return nullptr;
}

Expr *ObjectLiteralChecker::checkInferredKeys(ObjectLitExpr *literal, ObjectLitShape &shape) {
  auto fields = std::vector<Field>();
  for (ObjectLitExpr::Property &property : literal->props) {
    if (this->reportInferredProperty(property, fields)) {
      literal->type = Type::error();
      return literal;
    }
  }
  shape.target = Type::object(std::move(fields));
  shape.isInferred = true;

  return nullptr;
}

bool ObjectLiteralChecker::reportInferredProperty(ObjectLitExpr::Property &property,
                                                  std::vector<Field> &fields) {
  property.value = this->exprChecker->checkExpr(property.value);
  const Type *value = property.value->type;
  if (value->isError()) {
    return true;
  }
  auto known = this->checkInferredValue(value);
  if (known.isErr()) {
    this->state.getDiagnostics().error(property.value->loc(), known.error().message);
    return true;
  }

  return this->reportKeyedFields(property, value, fields);
}

bool ObjectLiteralChecker::reportKeyedFields(const ObjectLitExpr::Property &property,
                                             const Type *value, std::vector<Field> &fields) {
  const Type *keyType = property.key()->type;
  llvm::ArrayRef<const Type *> names =
      keyType->isUnion() ? keyType->members() : llvm::ArrayRef<const Type *>(keyType);
  for (const Type *nameType : names) {
    auto name = ToUtf8::toUtf8(nameType->literalText());
    const auto *same = this->fieldNamed(fields, name);
    bool isClashing = same != nullptr && this->reportKeyedClash(property, name, *same, value);
    if (isClashing) {
      return true;
    }
    if (same == nullptr) {
      fields.push_back({name, Type::optional(value), false});
    }
  }

  return false;
}

bool ObjectLiteralChecker::reportKeyedClash(const ObjectLitExpr::Property &property,
                                            llvm::StringRef name, const Field &same,
                                            const Type *value) {
  auto agree = this->checkSameKeyedType(name, same, value);
  if (agree.isOk()) {
    return false;
  }
  this->state.getDiagnostics().error(property.key()->loc(), agree.error().message);

  return true;
}

Expr *ObjectLiteralChecker::checkKeyedProperties(ObjectLitExpr *literal,
                                                 const ObjectLitShape &shape) {
  for (ObjectLitExpr::Property &property : literal->props) {
    auto *failed = property.keyed() ? this->checkKeyedProperty(literal, property, shape) : nullptr;
    if (failed != nullptr) {
      return failed;
    }
  }

  return nullptr;
}

Expr *ObjectLiteralChecker::checkKeyedProperty(ObjectLitExpr *literal,
                                               ObjectLitExpr::Property &property,
                                               const ObjectLitShape &shape) {
  const Type *keyType = property.key()->type;
  auto placed = this->checkKeyedPlacement(literal, keyType);
  if (placed.isErr()) {
    this->state.getDiagnostics().error(property.key()->loc(), placed.error().message);
    return literal;
  }
  auto object = this->checkKeyedTarget(shape.target, keyType);
  if (object.isErr()) {
    this->state.getDiagnostics().error(property.key()->loc(), object.error().message);
    property.value = this->exprChecker->checkExpr(property.value);
    literal->type = Type::error();
    return literal;
  }
  auto agree = this->checkKeyedField(shape.target, keyType);
  if (agree.isErr()) {
    this->state.getDiagnostics().error(property.key()->loc(), agree.error().message);
    property.value = this->exprChecker->checkExpr(property.value);
    literal->type = Type::error();
    return literal;
  }
  const Type *field = agree.value();
  auto what = ErrorTextObjectLiteral::keyedName(keyType->name());
  Expr *value =
      shape.isInferred ? property.value : this->exprChecker->checkExpr(property.value, field);
  property.value = this->typeRelations->coerce(value, field, what);

  return nullptr;
}

std::vector<Field> ObjectLiteralChecker::checkNamedProperties(ObjectLitExpr *literal,
                                                              ObjectLitShape &shape) {
  auto fields = std::vector<Field>();
  for (size_t index = 0; index < literal->props.size(); ++index) {
    ObjectLitExpr::Property &property = literal->props[index];
    if (!property.keyed()) {
      this->walkLiteralProperty(property, shape.named[index], shape, fields);
    }
  }

  return fields;
}

void ObjectLiteralChecker::walkLiteralProperty(ObjectLitExpr::Property &property, bool isNamed,
                                               ObjectLitShape &shape, std::vector<Field> &fields) {
  if (!isNamed) {
    property.value = this->exprChecker->checkExpr(property.value);
    return;
  }
  this->nameFunctionValue(property);
  if (shape.target != nullptr) {
    return this->walkTargetField(property, shape.target);
  }
  auto field = this->untargetedField(property, shape);
  fields.push_back(field);
}

void ObjectLiteralChecker::nameFunctionValue(ObjectLitExpr::Property &property) const {
  auto *functionExpr = dyn_cast<FunctionExpr>(property.value);
  if (functionExpr != nullptr && functionExpr->fn->name.empty()) {
    functionExpr->fn->name = property.name;
  }
}

bool ObjectLiteralChecker::takePrechecked(const Expr *value) {
  bool isPrechecked = this->state.getExpressionMarks().isPrechecked(value);
  this->state.getExpressionMarks().forgetPrechecked(value);

  return isPrechecked;
}

void ObjectLiteralChecker::walkTargetField(ObjectLitExpr::Property &property, const Type *target) {
  auto what = ErrorTextObjectLiteral::fieldName(property.name);
  auto found = this->checkTargetField(target, property.name);
  bool isPrechecked = this->takePrechecked(property.value);
  if (found.isErr()) {
    return this->walkMissingField(property, found.error(), isPrechecked);
  }
  const Field *field = found.value();
  Expr *value =
      isPrechecked ? property.value : this->exprChecker->checkExpr(property.value, field->type);
  property.value = this->typeRelations->coerce(value, field->type, what);
}

void ObjectLiteralChecker::walkMissingField(ObjectLitExpr::Property &property,
                                            const ObjectLiteralCheckerTargetFieldError &error,
                                            bool isPrechecked) {
  if (!isPrechecked) {
    property.value = this->exprChecker->checkExpr(property.value);
  }
  if (error.code != ObjectLiteralCheckerTargetFieldErrorCode::OVERLOAD_NAME) {
    this->state.getDiagnostics().error(property.loc, error.message);
  }
}

Field ObjectLiteralChecker::untargetedField(ObjectLitExpr::Property &property,
                                            ObjectLitShape &shape) {
  bool isPrechecked = this->takePrechecked(property.value);
  if (!isPrechecked) {
    property.value = this->exprChecker->checkExpr(property.value);
  }
  const Type *type = property.value->type;
  bool isUsable = !type->isError() && !this->reportUnusableValue(property, type);
  if (!isUsable) {
    shape.isTyped = false;
  }

  return Field{property.name.str(), type, false};
}

bool ObjectLiteralChecker::reportUnusableValue(const ObjectLitExpr::Property &property,
                                               const Type *type) {
  auto what = ErrorTextObjectLiteral::fieldName(property.name);
  auto usable = this->checkValueTyped(type, what, property.name);
  if (usable.isOk()) {
    return false;
  }
  this->state.getDiagnostics().error(property.value->loc(), usable.error().message);

  return true;
}

Expr *ObjectLiteralChecker::finishObjectLit(ObjectLitExpr *literal, const ObjectLitShape &shape,
                                            std::vector<Field> fields) {
  const Type *target = shape.target;
  if (target == nullptr) {
    literal->type = shape.isTyped ? Type::object(std::move(fields)) : Type::error();
    return literal;
  }
  for (const Field &field : target->fields()) {
    auto given = this->checkFieldGiven(literal, field, target);
    if (given.isErr()) {
      this->state.getDiagnostics().error(literal->loc(), given.error().message);
    }
  }
  for (const Overload &overload : target->overloads()) {
    auto message = ErrorTextObjectLiteral::overloads(target->name(), overload.name);
    this->state.getDiagnostics().error(literal->loc(), message);
  }
  literal->type = target;

  return literal;
}

ObjectLiteralChecker::ResultDefinedSpreading
ObjectLiteralChecker::checkSpreadDefined(const Type *type) const {
  if (type->isOptional() && type->inner()->isObject() && !type->inner()->isTuple()) {
    auto message = ErrorTextSpreadLiteral::undefined();

    return err(ObjectLiteralCheckerSpreadDefinedError{
        .code = ObjectLiteralCheckerSpreadDefinedErrorCode::MAY_BE_UNDEFINED,
        .message = message,
    });
  }

  return ok(type);
}

ObjectLiteralChecker::ResultSpreadIsAnObject
ObjectLiteralChecker::checkSpreadObject(const Type *type) const {
  bool isObject = type->isObject() && !type->isTuple() && !type->isRegExp() && !type->isMatch() &&
                  !type->isDate() && !type->isIntl() && !type->isBytes() && !type->isCrypto();
  if (!isObject) {
    bool hasIndices =
        type->nonOptional()->isArray() || type->nonOptional()->isTuple() || type->isString();
    auto message = ErrorTextSpreadLiteral::notObject(type->name(), hasIndices);

    return err(ObjectLiteralCheckerSpreadObjectError{
        .code = ObjectLiteralCheckerSpreadObjectErrorCode::NOT_AN_OBJECT,
        .message = message,
    });
  }

  return ok(type);
}

ObjectLiteralChecker::ResultPlainSpreadClass
ObjectLiteralChecker::checkSpreadFields(const ClassDecl *classDecl) const {
  if (this->classDeclarer->hasMethodFields(classDecl)) {
    auto message = ErrorTextSpreadLiteral::methods();

    return err(ObjectLiteralCheckerSpreadFieldsError{
        .code = ObjectLiteralCheckerSpreadFieldsErrorCode::METHOD_FIELDS,
        .message = message,
    });
  }

  return ok(classDecl);
}

ObjectLiteralChecker::ResultSpreadFieldJoins
ObjectLiteralChecker::checkSpreadJoin(const Field &spread, const Field &field,
                                      const Type *type) const {
  const auto *base = this->typeRelations->unify(field.type->nonOptional(), spread.type->inner());
  if (base == nullptr || base->isError()) {
    auto message = ErrorTextSpreadLiteral::twoTypes(spread.name, field.type->name(),
                                                    spread.type->name(), type->name());

    return err(ObjectLiteralCheckerSpreadJoinError{
        .code = ObjectLiteralCheckerSpreadJoinErrorCode::TYPES_DIFFER,
        .message = message,
    });
  }

  return ok(base);
}

Expr *ObjectLiteralChecker::checkSpreadLit(ObjectLitExpr *literal, const Type *target,
                                           llvm::ArrayRef<bool> named, bool isTyped) {
  auto collected = SpreadFields{
      .target = target,
      .fields = {},
      .hasRecords = false,
      .isTyped = isTyped,
  };
  for (size_t index = 0; index < literal->props.size(); ++index) {
    ObjectLitExpr::Property &property = literal->props[index];
    if (property.spread()) {
      this->walkSpreadProp(property, collected);
    } else {
      this->walkNamedProp(property, named[index], collected);
    }
  }
  if (collected.hasRecords && collected.isTyped) {
    return this->recordLiteralChecker->recordFromSpreads(literal);
  }
  if (!collected.isTyped) {
    literal->type = Type::error();
    return literal;
  }
  const auto *result = this->spreadResult(collected);
  this->convertSpreadProps(literal, result);
  literal->type = result;

  return literal;
}

void ObjectLiteralChecker::walkNamedProp(ObjectLitExpr::Property &property, bool isNamed,
                                         SpreadFields &collected) {
  if (!isNamed) {
    property.value = this->exprChecker->checkExpr(property.value);
    return;
  }
  this->nameFunctionValue(property);
  const Field *wanted =
      collected.target != nullptr ? collected.target->field(property.name) : nullptr;
  const Type *hint = wanted != nullptr ? wanted->type : nullptr;
  property.value = this->exprChecker->checkExpr(property.value, hint);
  const Type *type = property.value->type;
  auto *field = this->findSlot(collected.fields, property.name);
  if (type->isError()) {
    collected.isTyped = false;
    return;
  }
  bool isFilled = type->isUndefined() && field != nullptr;
  if (!isFilled && this->reportUnusableValue(property, type)) {
    collected.isTyped = false;
    return;
  }
  const Type *stored = isFilled ? Type::optional(field->type) : type;
  if (field != nullptr) {
    field->type = stored;
    return;
  }
  collected.fields.push_back({property.name.str(), stored, false});
}

void ObjectLiteralChecker::walkSpreadProp(ObjectLitExpr::Property &property,
                                          SpreadFields &collected) {
  property.value = this->exprChecker->checkExpr(property.value);
  const Type *type = property.value->type;
  if (type->isError()) {
    collected.isTyped = false;
    return;
  }
  if (type->isRecord() && collected.target == nullptr) {
    collected.hasRecords = true;
    return;
  }
  if (this->reportSpreadType(property, type)) {
    collected.isTyped = false;
    return;
  }
  this->takeOmitKeys(property);
  if (this->reportSpreadClass(property, type)) {
    collected.isTyped = false;
    return;
  }
  this->mergeSpreadFields(property, type, collected);
}

bool ObjectLiteralChecker::reportSpreadType(const ObjectLitExpr::Property &property,
                                            const Type *type) {
  auto defined = this->checkSpreadDefined(type);
  if (defined.isErr()) {
    this->state.getDiagnostics().error(property.value->loc(), defined.error().message);
    return true;
  }
  auto object = this->checkSpreadObject(type);
  if (object.isErr()) {
    this->state.getDiagnostics().error(property.value->loc(), object.error().message);
    return true;
  }

  return false;
}

bool ObjectLiteralChecker::reportSpreadClass(const ObjectLitExpr::Property &property,
                                             const Type *type) {
  auto *classDecl = this->classDeclarer->classOf(type);
  if (classDecl == nullptr) {
    return false;
  }
  this->classDeclarer->completeClass(classDecl);
  auto plain = this->checkSpreadFields(classDecl);
  if (plain.isErr()) {
    this->state.getDiagnostics().error(property.value->loc(), plain.error().message);
    return true;
  }

  return false;
}

void ObjectLiteralChecker::mergeSpreadFields(const ObjectLitExpr::Property &property,
                                             const Type *type, SpreadFields &collected) {
  for (const Field &spreadField : type->fields()) {
    if (!llvm::is_contained(property.omit(), spreadField.name)) {
      this->mergeSpreadField(property, spreadField, type, collected);
    }
  }
}

void ObjectLiteralChecker::mergeSpreadField(const ObjectLitExpr::Property &property,
                                            const Field &spreadField, const Type *type,
                                            SpreadFields &collected) {
  auto *field = this->findSlot(collected.fields, spreadField.name);
  if (field == nullptr) {
    collected.fields.push_back({spreadField.name, spreadField.type, false});
    return;
  }
  if (!spreadField.type->isOptional()) {
    field->type = spreadField.type;
    return;
  }
  auto joined = this->checkSpreadJoin(spreadField, *field, type);
  if (joined.isErr()) {
    this->state.getDiagnostics().error(property.loc, joined.error().message);
    collected.isTyped = false;
    return;
  }
  const Type *base = joined.value();
  field->type = field->type->isOptional() ? Type::optional(base) : base;
}

Field *ObjectLiteralChecker::findSlot(std::vector<Field> &fields, llvm::StringRef name) const {
  for (Field &field : fields) {
    if (field.name == name) {
      return &field;
    }
  }

  return nullptr;
}

void ObjectLiteralChecker::takeOmitKeys(ObjectLitExpr::Property &property) const {
  SpreadProperty *spread = property.form.as<SpreadProperty>();
  for (const IndexExpr *read : spread->omitKeys) {
    if (!read->field().empty()) {
      spread->omit.push_back(read->field());
    }
  }
  spread->omitKeys.clear();
}

const Type *ObjectLiteralChecker::spreadResult(const SpreadFields &collected) const {
  const auto *result = Type::object(collected.fields);
  if (collected.target == nullptr) {
    return result;
  }
  bool isFitting = this->isEveryFieldFitting(collected.fields, collected.target) &&
                   this->isTargetFilled(collected.target, collected.fields);

  return isFitting ? collected.target : result;
}

bool ObjectLiteralChecker::isEveryFieldFitting(const std::vector<Field> &fields,
                                               const Type *target) const {
  for (const Field &field : fields) {
    const auto *targetField = target->field(field.name);
    bool isFitting = targetField != nullptr &&
                     TypeRelations::isImplicitlyConvertible(field.type, targetField->type);
    if (!isFitting) {
      return false;
    }
  }

  return true;
}

bool ObjectLiteralChecker::isTargetFilled(const Type *target,
                                          const std::vector<Field> &fields) const {
  for (const Field &targetField : target->fields()) {
    if (!targetField.type->isOptional() && this->fieldNamed(fields, targetField.name) == nullptr) {
      return false;
    }
  }

  return true;
}

void ObjectLiteralChecker::convertSpreadProps(ObjectLitExpr *literal, const Type *result) {
  for (size_t index = 0; index < literal->props.size(); ++index) {
    this->convertSpreadProp(literal, index, result);
  }
}

void ObjectLiteralChecker::convertSpreadProp(ObjectLitExpr *literal, size_t index,
                                             const Type *result) {
  ObjectLitExpr::Property &property = literal->props[index];
  if (property.spread()) {
    return this->markReplacedFields(literal, index);
  }
  if (this->isReplacedLater(literal, index, property.name)) {
    property.replaced = true;
    return;
  }
  const Type *fieldType = result->field(property.name)->type;
  property.value = this->typeRelations->convert(property.value, fieldType);
}

void ObjectLiteralChecker::markReplacedFields(ObjectLitExpr *literal, size_t index) const {
  ObjectLitExpr::Property &property = literal->props[index];
  SpreadProperty *spread = property.form.as<SpreadProperty>();
  for (const Field &field : property.value->type->fields()) {
    bool isOmitted = llvm::is_contained(spread->omit, field.name);
    if (!isOmitted && this->isReplacedLater(literal, index, field.name)) {
      spread->replacedFields.push_back(field.name);
    }
  }
}

bool ObjectLiteralChecker::isReplacedLater(const ObjectLitExpr *literal, size_t index,
                                           llvm::StringRef name) const {
  for (size_t laterIndex = index + 1; laterIndex < literal->props.size(); ++laterIndex) {
    if (this->isReplacing(literal->props[laterIndex], name)) {
      return true;
    }
  }

  return false;
}

bool ObjectLiteralChecker::isReplacing(const ObjectLitExpr::Property &later,
                                       llvm::StringRef name) const {
  if (!later.spread()) {
    return later.name == name;
  }
  if (llvm::is_contained(later.omit(), name)) {
    return false;
  }
  const auto *field = later.value->type->field(name);

  return field != nullptr && !field->type->isOptional();
}

}
