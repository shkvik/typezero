#include "compiler/semantic/bodies/objects/object_literal_checker/object_literal_checker.h"
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

bool ObjectLiteralChecker::hasMethodFields(const ClassDecl *classDecl) const {
  if (classDecl->literal() != nullptr) {
    return true;
  }
  for (const ClassDecl *c = classDecl; c != nullptr; c = c->parent) {
    if (llvm::any_of(c->fields, [](const ClassField &field) { return field.method(); })) {
      return true;
    }
  }

  return false;
}

ObjectLiteralChecker::ResultPropertyHasValue
ObjectLiteralChecker::propertyPlain(const ObjectLitExpr::Property &property) const {
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
ObjectLiteralChecker::computedKey(const Type *keyType) const {
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

ObjectLiteralChecker::ResultFieldNameIsFresh
ObjectLiteralChecker::uniqueName(const ObjectLitExpr *literal, size_t index,
                                 llvm::ArrayRef<bool> named) const {
  for (size_t j = 0; j < index && named[index]; ++j) {
    if (named[j] && literal->props[j].name == literal->props[index].name) {
      auto message = ErrorTextObjectLiteral::duplicate(literal->props[index].name);

      return err(ObjectLiteralCheckerUniqueNameError{
          .code = ObjectLiteralCheckerUniqueNameErrorCode::DUPLICATE_FIELD,
          .message = message,
      });
    }
  }

  return ok(&literal->props[index]);
}

ObjectLiteralChecker::ResultValueTypeIsKnown
ObjectLiteralChecker::inferredValue(const Type *value) const {
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
ObjectLiteralChecker::sameKeyedType(llvm::StringRef name, const Field &same,
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

ObjectLiteralChecker::ResultKeyWithoutSpread
ObjectLiteralChecker::keyedPlacement(const ObjectLitExpr *literal, const Type *keyType) const {
  bool hasMethods = llvm::any_of(
      literal->props, [](const ObjectLitExpr::Property &property) { return property.method(); });
  if (literal->hasSpread() || hasMethods) {
    auto message = ErrorTextObjectLiteral::keySpread(keyType->name());

    return err(ObjectLiteralCheckerKeyedPlacementError{
        .code = ObjectLiteralCheckerKeyedPlacementErrorCode::KEY_WITH_SPREAD,
        .message = message,
    });
  }

  return ok(keyType);
}

ObjectLiteralChecker::ResultKeyedObjectTyped
ObjectLiteralChecker::keyedTarget(const Type *target, const Type *keyType) const {
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
ObjectLiteralChecker::keyedField(const Type *target, const Type *keyType) const {
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

ObjectLiteralChecker::ResultTargetFieldFound
ObjectLiteralChecker::targetField(const Type *target, llvm::StringRef name) const {
  const auto *field = target->field(name);
  if (field == nullptr && target->overload(name) != nullptr) {
    return err(ObjectLiteralCheckerTargetFieldError{
        .code = ObjectLiteralCheckerTargetFieldErrorCode::OVERLOAD_NAME,
        .message = "",
    });
  }
  if (field == nullptr) {
    bool isStream = target->name() == "StreamError";
    auto message = ErrorTextObjectLiteral::noField(target->name(), name, isStream);

    return err(ObjectLiteralCheckerTargetFieldError{
        .code = ObjectLiteralCheckerTargetFieldErrorCode::NO_SUCH_FIELD,
        .message = message,
    });
  }

  return ok(field);
}

ObjectLiteralChecker::ResultUsableFieldValue
ObjectLiteralChecker::valueTyped(const Type *type, llvm::StringRef what,
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

ObjectLiteralChecker::ResultRequiredFieldSet
ObjectLiteralChecker::fieldGiven(const ObjectLitExpr *literal, const Field &field,
                                 const Type *target) const {
  bool isGiven = llvm::any_of(literal->props, [&](const ObjectLitExpr::Property &property) {
    return property.name == field.name;
  });
  if (!isGiven && !field.type->isOptional()) {
    auto message = ErrorTextObjectLiteral::missing(field.name, target->name());

    return err(ObjectLiteralCheckerFieldGivenError{
        .code = ObjectLiteralCheckerFieldGivenErrorCode::MISSING_FIELD,
        .message = message,
    });
  }

  return ok(&field);
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
  ObjectLitShape shape{
      .target = this->isUsableTarget(wanted) ? wanted : nullptr,
      .named = llvm::SmallVector<bool, 8>(literal->props.size(), true),
      .typed = true,
      .inferred = false,
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
    return this->checkSpreadLit(literal, shape.target, shape.named, shape.typed);
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
  auto plain = this->propertyPlain(property);
  if (plain.isErr()) {
    this->state.getDiagnostics().error(property.loc, plain.error().message);
    property.value = cast<AssignExpr>(property.value)->value;
    property.form = ValueProperty{};
    shape.typed = false;
  }
  ComputedProperty *computed = property.form.as<ComputedProperty>();
  if (computed == nullptr) {
    return;
  }
  computed->key = this->exprChecker->checkExpr(computed->key);
  const Type *keyType = property.key()->type;
  auto name = this->keyedFieldChecker->constantKey(property.key());
  if (name.has_value()) {
    property.name = this->state.getAst().keepName(std::move(*name));
    this->state.getInventoryStore().addFieldName(property.name);
    return;
  }
  shape.named[index] = false;
  if (this->isLiteralKeys(keyType)) {
    computed->keyed = true;
    return;
  }
  shape.typed = false;
  if (keyType->isError()) {
    return;
  }
  auto key = this->computedKey(keyType);
  if (key.isErr()) {
    this->state.getDiagnostics().error(property.key()->loc(), key.error().message);
  }
}

bool ObjectLiteralChecker::isLiteralKeys(const Type *keyType) const {
  llvm::ArrayRef<const Type *> names =
      keyType->isUnion() ? keyType->members() : llvm::ArrayRef<const Type *>(keyType);
  for (const Type *n : names) {
    if (!n->isStringLiteral()) {
      return false;
    }
  }

  return true;
}

void ObjectLiteralChecker::walkUniqueNames(ObjectLitExpr *literal, ObjectLitShape &shape) {
  for (size_t index = 0; index < literal->props.size(); ++index) {
    auto fresh = this->uniqueName(literal, index, shape.named);
    if (fresh.isErr()) {
      this->state.getDiagnostics().error(literal->props[index].loc, fresh.error().message);
      shape.typed = false;
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

Expr *ObjectLiteralChecker::checkInferredKeys(ObjectLitExpr *literal, ObjectLitShape &shape) {
  auto fields = std::vector<Field>();
  for (ObjectLitExpr::Property &property : literal->props) {
    property.value = this->exprChecker->checkExpr(property.value);
    const Type *value = property.value->type;
    if (value->isError()) {
      literal->type = Type::error();
      return literal;
    }
    auto known = this->inferredValue(value);
    if (known.isErr()) {
      this->state.getDiagnostics().error(property.value->loc(), known.error().message);
      literal->type = Type::error();
      return literal;
    }
    if (!this->keyedFieldsAgree(property, value, fields)) {
      literal->type = Type::error();
      return literal;
    }
  }
  shape.target = Type::object(std::move(fields));
  shape.inferred = true;

  return nullptr;
}

bool ObjectLiteralChecker::keyedFieldsAgree(const ObjectLitExpr::Property &property,
                                            const Type *value, std::vector<Field> &fields) {
  const Type *keyType = property.key()->type;
  llvm::ArrayRef<const Type *> names =
      keyType->isUnion() ? keyType->members() : llvm::ArrayRef<const Type *>(keyType);
  for (const Type *n : names) {
    auto name = ToUtf8::toUtf8(n->literalText());
    const auto *same = this->fieldNamed(fields, name);
    if (same == nullptr) {
      fields.push_back({name, Type::optional(value), false});
      continue;
    }
    auto agree = this->sameKeyedType(name, *same, value);
    if (agree.isErr()) {
      this->state.getDiagnostics().error(property.key()->loc(), agree.error().message);
      return false;
    }
  }

  return true;
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

Expr *ObjectLiteralChecker::checkKeyedProperties(ObjectLitExpr *literal,
                                                 const ObjectLitShape &shape) {
  for (ObjectLitExpr::Property &property : literal->props) {
    if (!property.keyed()) {
      continue;
    }
    auto *failed = this->checkKeyedProperty(literal, property, shape);
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
  auto placed = this->keyedPlacement(literal, keyType);
  if (placed.isErr()) {
    this->state.getDiagnostics().error(property.key()->loc(), placed.error().message);
    return literal;
  }
  auto object = this->keyedTarget(shape.target, keyType);
  if (object.isErr()) {
    this->state.getDiagnostics().error(property.key()->loc(), object.error().message);
    property.value = this->exprChecker->checkExpr(property.value);
    literal->type = Type::error();
    return literal;
  }
  auto agree = this->keyedField(shape.target, keyType);
  if (agree.isErr()) {
    this->state.getDiagnostics().error(property.key()->loc(), agree.error().message);
    property.value = this->exprChecker->checkExpr(property.value);
    literal->type = Type::error();
    return literal;
  }
  const Type *field = agree.value();
  auto what = ErrorTextObjectLiteral::keyedName(keyType->name());
  Expr *value =
      shape.inferred ? property.value : this->exprChecker->checkExpr(property.value, field);
  property.value = this->typeRelations->coerce(value, field, what);

  return nullptr;
}

std::vector<Field> ObjectLiteralChecker::checkNamedProperties(ObjectLitExpr *literal,
                                                              ObjectLitShape &shape) {
  auto fields = std::vector<Field>();
  for (size_t index = 0; index < literal->props.size(); ++index) {
    ObjectLitExpr::Property &property = literal->props[index];
    if (property.keyed()) {
      continue;
    }
    if (!shape.named[index]) {
      property.value = this->exprChecker->checkExpr(property.value);
      continue;
    }
    auto *functionExpr = dyn_cast<FunctionExpr>(property.value);
    if (functionExpr != nullptr && functionExpr->fn->name.empty()) {
      functionExpr->fn->name = property.name;
    }
    if (shape.target != nullptr) {
      this->walkTargetField(property, shape.target);
      continue;
    }
    fields.push_back(this->untargetedField(property, shape));
  }

  return fields;
}

bool ObjectLiteralChecker::takePrechecked(const Expr *value) {
  bool isPrechecked = this->state.getExpressionMarks().isPrechecked(value);
  this->state.getExpressionMarks().forgetPrechecked(value);

  return isPrechecked;
}

void ObjectLiteralChecker::walkTargetField(ObjectLitExpr::Property &property, const Type *target) {
  auto what = ErrorTextObjectLiteral::fieldName(property.name);
  auto found = this->targetField(target, property.name);
  bool isPrechecked = this->takePrechecked(property.value);
  if (found.isErr()) {
    if (!isPrechecked) {
      property.value = this->exprChecker->checkExpr(property.value);
    }
    if (found.error().code != ObjectLiteralCheckerTargetFieldErrorCode::OVERLOAD_NAME) {
      this->state.getDiagnostics().error(property.loc, found.error().message);
    }
    return;
  }
  const Field *field = found.value();
  Expr *value =
      isPrechecked ? property.value : this->exprChecker->checkExpr(property.value, field->type);
  property.value = this->typeRelations->coerce(value, field->type, what);
}

Field ObjectLiteralChecker::untargetedField(ObjectLitExpr::Property &property,
                                            ObjectLitShape &shape) {
  auto what = ErrorTextObjectLiteral::fieldName(property.name);
  bool isPrechecked = this->takePrechecked(property.value);
  if (!isPrechecked) {
    property.value = this->exprChecker->checkExpr(property.value);
  }
  const Type *type = property.value->type;
  if (type->isError()) {
    shape.typed = false;
    return Field{property.name.str(), type, false};
  }
  auto usable = this->valueTyped(type, what, property.name);
  if (usable.isErr()) {
    this->state.getDiagnostics().error(property.value->loc(), usable.error().message);
    shape.typed = false;
  }

  return Field{property.name.str(), type, false};
}

Expr *ObjectLiteralChecker::finishObjectLit(ObjectLitExpr *literal, const ObjectLitShape &shape,
                                            std::vector<Field> fields) {
  const Type *target = shape.target;
  if (target == nullptr) {
    literal->type = shape.typed ? Type::object(std::move(fields)) : Type::error();
    return literal;
  }
  for (const Field &field : target->fields()) {
    auto given = this->fieldGiven(literal, field, target);
    if (given.isErr()) {
      this->state.getDiagnostics().error(literal->loc(), given.error().message);
    }
  }
  for (const Overload &o : target->overloads()) {
    auto message = ErrorTextObjectLiteral::overloads(target->name(), o.name);
    this->state.getDiagnostics().error(literal->loc(), message);
  }
  literal->type = target;

  return literal;
}

ObjectLiteralChecker::ResultDefinedSpreading
ObjectLiteralChecker::spreadDefined(const Type *type) const {
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
ObjectLiteralChecker::spreadObject(const Type *type) const {
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
ObjectLiteralChecker::spreadFields(const ClassDecl *classDecl) const {
  if (this->hasMethodFields(classDecl)) {
    auto message = ErrorTextSpreadLiteral::methods();

    return err(ObjectLiteralCheckerSpreadFieldsError{
        .code = ObjectLiteralCheckerSpreadFieldsErrorCode::METHOD_FIELDS,
        .message = message,
    });
  }

  return ok(classDecl);
}

ObjectLiteralChecker::ResultSpreadFieldJoins
ObjectLiteralChecker::spreadJoin(const Field &spread, const Field &field, const Type *type) const {
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
                                           llvm::ArrayRef<bool> named, bool typed) {
  SpreadFields collected{.target = target, .fields = {}, .records = false, .typed = typed};
  for (size_t index = 0; index < literal->props.size(); ++index) {
    ObjectLitExpr::Property &property = literal->props[index];
    if (property.spread()) {
      this->walkSpreadProp(property, collected);
    } else {
      this->walkNamedProp(property, named[index], collected);
    }
  }
  if (collected.records && collected.typed) {
    return this->recordLiteralChecker->recordFromSpreads(literal);
  }
  if (!collected.typed) {
    literal->type = Type::error();
    return literal;
  }
  const auto *result = this->spreadResult(collected);
  this->convertSpreadProps(literal, result);
  literal->type = result;

  return literal;
}

Field *ObjectLiteralChecker::findSlot(std::vector<Field> &fields, llvm::StringRef name) const {
  for (Field &field : fields) {
    if (field.name == name) {
      return &field;
    }
  }

  return nullptr;
}

void ObjectLiteralChecker::walkNamedProp(ObjectLitExpr::Property &property, bool isNamed,
                                         SpreadFields &collected) {
  if (!isNamed) {
    property.value = this->exprChecker->checkExpr(property.value);
    return;
  }
  auto *functionExpr = dyn_cast<FunctionExpr>(property.value);
  if (functionExpr != nullptr && functionExpr->fn->name.empty()) {
    functionExpr->fn->name = property.name;
  }
  const Field *wanted =
      collected.target != nullptr ? collected.target->field(property.name) : nullptr;
  property.value =
      this->exprChecker->checkExpr(property.value, wanted != nullptr ? wanted->type : nullptr);
  const Type *type = property.value->type;
  auto *field = this->findSlot(collected.fields, property.name);
  auto what = ErrorTextObjectLiteral::fieldName(property.name);
  if (type->isError()) {
    collected.typed = false;
    return;
  }
  bool isFilled = type->isUndefined() && field != nullptr;
  auto usableValue =
      isFilled ? ResultUsableFieldValue(ok(type)) : this->valueTyped(type, what, property.name);
  if (usableValue.isErr()) {
    this->state.getDiagnostics().error(property.value->loc(), usableValue.error().message);
    collected.typed = false;
    return;
  }
  if (isFilled) {
    type = Type::optional(field->type);
  }
  if (field != nullptr) {
    field->type = type;
  } else {
    collected.fields.push_back({property.name.str(), type, false});
  }
}

void ObjectLiteralChecker::walkSpreadProp(ObjectLitExpr::Property &property,
                                          SpreadFields &collected) {
  property.value = this->exprChecker->checkExpr(property.value);
  const Type *type = property.value->type;
  if (type->isError()) {
    collected.typed = false;
    return;
  }
  if (type->isRecord() && collected.target == nullptr) {
    collected.records = true;
    return;
  }
  if (!this->spreadTypeFits(property, type)) {
    collected.typed = false;
    return;
  }
  this->takeOmitKeys(property);
  if (!this->spreadClassFits(property, type)) {
    collected.typed = false;
    return;
  }
  this->mergeSpreadFields(property, type, collected);
}

bool ObjectLiteralChecker::spreadTypeFits(const ObjectLitExpr::Property &property,
                                          const Type *type) {
  auto defined = this->spreadDefined(type);
  if (defined.isErr()) {
    this->state.getDiagnostics().error(property.value->loc(), defined.error().message);
    return false;
  }
  auto object = this->spreadObject(type);
  if (object.isErr()) {
    this->state.getDiagnostics().error(property.value->loc(), object.error().message);
    return false;
  }

  return true;
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

bool ObjectLiteralChecker::spreadClassFits(const ObjectLitExpr::Property &property,
                                           const Type *type) {
  auto *classDecl = this->classDeclarer->classOf(type);
  if (classDecl == nullptr) {
    return true;
  }
  this->classDeclarer->completeClass(classDecl);
  auto plain = this->spreadFields(classDecl);
  if (plain.isErr()) {
    this->state.getDiagnostics().error(property.value->loc(), plain.error().message);
    return false;
  }

  return true;
}

void ObjectLiteralChecker::mergeSpreadFields(const ObjectLitExpr::Property &property,
                                             const Type *type, SpreadFields &collected) {
  for (const Field &f : type->fields()) {
    if (llvm::is_contained(property.omit(), f.name)) {
      continue;
    }
    auto *field = this->findSlot(collected.fields, f.name);
    if (field == nullptr) {
      collected.fields.push_back({f.name, f.type, false});
      continue;
    }
    if (!f.type->isOptional()) {
      field->type = f.type;
      continue;
    }
    auto joined = this->spreadJoin(f, *field, type);
    if (joined.isErr()) {
      this->state.getDiagnostics().error(property.loc, joined.error().message);
      collected.typed = false;
      continue;
    }
    const Type *base = joined.value();
    field->type = field->type->isOptional() ? Type::optional(base) : base;
  }
}

const Type *ObjectLiteralChecker::spreadResult(const SpreadFields &collected) const {
  const auto *result = Type::object(collected.fields);
  if (collected.target == nullptr) {
    return result;
  }
  bool isFitting = this->fieldsFitTarget(collected.fields, collected.target) &&
                   this->targetFilled(collected.target, collected.fields);

  return isFitting ? collected.target : result;
}

bool ObjectLiteralChecker::fieldsFitTarget(const std::vector<Field> &fields,
                                           const Type *target) const {
  for (const Field &field : fields) {
    const auto *t = target->field(field.name);
    if (t == nullptr || !TypeRelations::isImplicitlyConvertible(field.type, t->type)) {
      return false;
    }
  }

  return true;
}

bool ObjectLiteralChecker::targetFilled(const Type *target,
                                        const std::vector<Field> &fields) const {
  for (const Field &t : target->fields()) {
    if (!t.type->isOptional() && this->fieldNamed(fields, t.name) == nullptr) {
      return false;
    }
  }

  return true;
}

void ObjectLiteralChecker::convertSpreadProps(ObjectLitExpr *literal, const Type *result) {
  for (size_t index = 0; index < literal->props.size(); ++index) {
    ObjectLitExpr::Property &property = literal->props[index];
    if (property.spread()) {
      this->markReplacedFields(literal, index);
      continue;
    }
    if (this->isReplacedLater(literal, index, property.name)) {
      property.replaced = true;
      continue;
    }
    property.value =
        this->typeRelations->convert(property.value, result->field(property.name)->type);
  }
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
  for (size_t j = index + 1; j < literal->props.size(); ++j) {
    const ObjectLitExpr::Property &later = literal->props[j];
    if (!later.spread()) {
      if (later.name == name) {
        return true;
      }
      continue;
    }
    if (llvm::is_contained(later.omit(), name)) {
      continue;
    }
    const auto *field = later.value->type->field(name);
    if (field != nullptr && !field->type->isOptional()) {
      return true;
    }
  }

  return false;
}

}
