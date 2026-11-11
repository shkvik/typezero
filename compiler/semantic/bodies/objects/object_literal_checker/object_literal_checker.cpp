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

bool ObjectLiteralChecker::hasMethodFields(const ClassDecl *cls) {
  if (cls->literal()) {
    return true;
  }
  for (const ClassDecl *c = cls; c; c = c->parent) {
    if (llvm::any_of(c->fields, [](const ClassField &f) { return f.method(); })) {
      return true;
    }
  }

  return false;
}

ObjectLiteralChecker::ResultPropertyHasValue
ObjectLiteralChecker::propertyPlain(const ObjectLitExpr::Property &prop) {
  if (prop.defaulted()) {
    std::string message = ErrorTextObjectLiteral::defaulted(prop.name);

    return err(ObjectLiteralCheckerPropertyPlainError{
        .code = ObjectLiteralCheckerPropertyPlainErrorCode::DEFAULT_VALUE,
        .message = message,
    });
  }

  return ok(&prop);
}

ObjectLiteralChecker::ResultComputedKeyKnown
ObjectLiteralChecker::computedKey(const Type *keyType) {
  if (keyType->nonOptional()->isNumeric()) {
    std::string message = ErrorTextObjectLiteral::numberKey();

    return err(ObjectLiteralCheckerComputedKeyError{
        .code = ObjectLiteralCheckerComputedKeyErrorCode::NUMBER_KEY,
        .message = message,
    });
  }
  if (keyType->isString()) {
    std::string message = ErrorTextObjectLiteral::runtimeKey();

    return err(ObjectLiteralCheckerComputedKeyError{
        .code = ObjectLiteralCheckerComputedKeyErrorCode::RUNTIME_KEY,
        .message = message,
    });
  }
  std::string message = ErrorTextObjectLiteral::keyType(keyType->name());

  return err(ObjectLiteralCheckerComputedKeyError{
      .code = ObjectLiteralCheckerComputedKeyErrorCode::NOT_A_STRING,
      .message = message,
  });
}

ObjectLiteralChecker::ResultFieldNameIsFresh
ObjectLiteralChecker::uniqueName(const ObjectLitExpr *lit, size_t i, llvm::ArrayRef<bool> named) {
  for (size_t j = 0; j < i && named[i]; ++j) {
    if (named[j] && lit->props[j].name == lit->props[i].name) {
      std::string message = ErrorTextObjectLiteral::duplicate(lit->props[i].name);

      return err(ObjectLiteralCheckerUniqueNameError{
          .code = ObjectLiteralCheckerUniqueNameErrorCode::DUPLICATE_FIELD,
          .message = message,
      });
    }
  }

  return ok(&lit->props[i]);
}

ObjectLiteralChecker::ResultValueTypeIsKnown
ObjectLiteralChecker::inferredValue(const Type *value) {
  if (value->isVoid() || value->isUndefined()) {
    std::string message = ErrorTextObjectLiteral::inferFrom(value->name());

    return err(ObjectLiteralCheckerInferredValueError{
        .code = ObjectLiteralCheckerInferredValueErrorCode::CANNOT_INFER,
        .message = message,
    });
  }

  return ok(value);
}

ObjectLiteralChecker::ResultKeyedValuesAgree
ObjectLiteralChecker::sameKeyedType(llvm::StringRef name, const Field &same, const Type *value) {
  if (same.type != Type::optional(value)) {
    std::string message = ErrorTextObjectLiteral::keyTypes(name);

    return err(ObjectLiteralCheckerSameKeyedTypeError{
        .code = ObjectLiteralCheckerSameKeyedTypeErrorCode::FIELD_TYPES_DIFFER,
        .message = message,
    });
  }

  return ok(value);
}

ObjectLiteralChecker::ResultKeyWithoutSpread
ObjectLiteralChecker::keyedPlacement(const ObjectLitExpr *lit, const Type *keyType) {
  bool hasMethods =
      llvm::any_of(lit->props, [](const ObjectLitExpr::Property &p) { return p.method(); });
  if (lit->hasSpread() || hasMethods) {
    std::string message = ErrorTextObjectLiteral::keySpread(keyType->name());

    return err(ObjectLiteralCheckerKeyedPlacementError{
        .code = ObjectLiteralCheckerKeyedPlacementErrorCode::KEY_WITH_SPREAD,
        .message = message,
    });
  }

  return ok(keyType);
}

ObjectLiteralChecker::ResultKeyedObjectTyped
ObjectLiteralChecker::keyedTarget(const Type *target, const Type *keyType) {
  if (!target) {
    std::string message = ErrorTextObjectLiteral::keyNoType(keyType->name());

    return err(ObjectLiteralCheckerKeyedTargetError{
        .code = ObjectLiteralCheckerKeyedTargetErrorCode::NO_OBJECT_TYPE,
        .message = message,
    });
  }

  return ok(target);
}

ObjectLiteralChecker::ResultKeyedFieldsAgree ObjectLiteralChecker::keyedField(const Type *target,
                                                                              const Type *keyType) {
  const Type *field = KeyedFieldChecker::keyedFieldType(target, keyType);
  if (!field) {
    std::string message = ErrorTextObjectLiteral::keyFields(keyType->name(), target->name());

    return err(ObjectLiteralCheckerKeyedFieldError{
        .code = ObjectLiteralCheckerKeyedFieldErrorCode::FIELDS_DIFFER,
        .message = message,
    });
  }
  if (!field->isOptional()) {
    std::string message = ErrorTextObjectLiteral::keyedOpt(keyType->name(), field->name());

    return err(ObjectLiteralCheckerKeyedFieldError{
        .code = ObjectLiteralCheckerKeyedFieldErrorCode::NOT_OPTIONAL,
        .message = message,
    });
  }

  return ok(field);
}

ObjectLiteralChecker::ResultTargetFieldFound
ObjectLiteralChecker::targetField(const Type *target, llvm::StringRef name) {
  const Field *field = target->field(name);
  if (!field && target->overload(name)) {
    return err(ObjectLiteralCheckerTargetFieldError{
        .code = ObjectLiteralCheckerTargetFieldErrorCode::OVERLOAD_NAME,
        .message = "",
    });
  }
  if (!field) {
    bool isStream = target->name() == "StreamError";
    std::string message = ErrorTextObjectLiteral::noField(target->name(), name, isStream);

    return err(ObjectLiteralCheckerTargetFieldError{
        .code = ObjectLiteralCheckerTargetFieldErrorCode::NO_SUCH_FIELD,
        .message = message,
    });
  }

  return ok(field);
}

ObjectLiteralChecker::ResultUsableFieldValue
ObjectLiteralChecker::valueTyped(const Type *type, llvm::StringRef what, llvm::StringRef name) {
  if (type->isVoid()) {
    std::string message = ErrorTextObjectLiteral::voidValue(what);

    return err(ObjectLiteralCheckerValueTypedError{
        .code = ObjectLiteralCheckerValueTypedErrorCode::VOID_VALUE,
        .message = message,
    });
  }
  if (type->isUndefined()) {
    std::string message = ErrorTextObjectLiteral::undefOf(what, name);

    return err(ObjectLiteralCheckerValueTypedError{
        .code = ObjectLiteralCheckerValueTypedErrorCode::UNDEFINED_VALUE,
        .message = message,
    });
  }

  return ok(type);
}

ObjectLiteralChecker::ResultRequiredFieldSet
ObjectLiteralChecker::fieldGiven(const ObjectLitExpr *lit, const Field &field, const Type *target) {
  bool isGiven = llvm::any_of(
      lit->props, [&](const ObjectLitExpr::Property &p) { return p.name == field.name; });
  if (!isGiven && !field.type->isOptional()) {
    std::string message = ErrorTextObjectLiteral::missing(field.name, target->name());

    return err(ObjectLiteralCheckerFieldGivenError{
        .code = ObjectLiteralCheckerFieldGivenErrorCode::MISSING_FIELD,
        .message = message,
    });
  }

  return ok(&field);
}

Expr *ObjectLiteralChecker::checkObjectLit(ObjectLitExpr *lit, const Type *expected) {
  const Type *wanted = expected != nullptr ? expected->nonOptional() : nullptr;
  if (wanted != nullptr && wanted->isJson()) {
    return this->jsonChecker->checkJsonObjectLit(lit);
  }
  if (wanted != nullptr && wanted->isRecord()) {
    return this->recordLiteralChecker->checkRecordLit(lit, wanted);
  }
  const Type *expectedUnion = wanted != nullptr && wanted->isUnion() ? wanted : nullptr;
  ObjectLitShape shape{
      .target = ObjectLiteralChecker::isUsableTarget(wanted) ? wanted : nullptr,
      .named = llvm::SmallVector<bool, 8>(lit->props.size(), true),
      .typed = true,
      .inferred = false,
  };
  for (size_t i = 0; i < lit->props.size(); ++i) {
    this->walkPropertyKey(lit, i, shape);
  }
  this->walkUniqueNames(lit, shape);
  Expr *record =
      expectedUnion != nullptr ? this->pickUnionTarget(lit, expectedUnion, shape) : nullptr;
  if (record != nullptr) {
    return record;
  }
  if (shape.target != nullptr) {
    ObjectLiteralChecker::walkThisMethods(lit, shape.target);
  }
  bool isKeyedOnly =
      shape.target == nullptr && !lit->props.empty() && ObjectLiteralChecker::isAllKeyed(lit);
  Expr *inferFailed = isKeyedOnly ? this->checkInferredKeys(lit, shape) : nullptr;
  if (inferFailed != nullptr) {
    return inferFailed;
  }
  Expr *keyedFailed = this->checkKeyedProperties(lit, shape);
  if (keyedFailed != nullptr) {
    return keyedFailed;
  }
  if (ObjectLiteralChecker::hasMethod(lit)) {
    return this->methodLiteralChecker->checkMethodLiteral(lit, shape.target);
  }
  if (lit->hasSpread()) {
    return this->checkSpreadLit(lit, shape.target, shape.named, shape.typed);
  }
  std::vector<Field> fields = this->checkNamedProperties(lit, shape);

  return this->finishObjectLit(lit, shape, std::move(fields));
}

bool ObjectLiteralChecker::isUsableTarget(const Type *t) {
  return t != nullptr && t->isObject() && !t->isIncomplete() && !t->isTuple() && !t->isClass() &&
         !t->isRegExp() && !t->isMatch() && !t->isDate() && !t->isIntl() && !t->isBytes() &&
         !t->isCrypto() && !t->isAnyObject();
}

void ObjectLiteralChecker::walkPropertyKey(ObjectLitExpr *lit, size_t i, ObjectLitShape &shape) {
  ObjectLitExpr::Property &prop = lit->props[i];
  shape.named[i] = !prop.spread();
  ResultPropertyHasValue plain = ObjectLiteralChecker::propertyPlain(prop);
  if (plain.isErr()) {
    this->state.getDiagnostics().error(prop.loc, plain.error().message);
    prop.value = cast<AssignExpr>(prop.value)->value;
    prop.form = ValueProperty{};
    shape.typed = false;
  }
  ComputedProperty *computed = prop.form.as<ComputedProperty>();
  if (computed == nullptr) {
    return;
  }
  computed->key = this->exprChecker->checkExpr(computed->key);
  const Type *keyType = prop.key()->type;
  std::optional<std::string> name = this->keyedFieldChecker->constantKey(prop.key());
  if (name.has_value()) {
    prop.name = this->state.getAst().keepName(std::move(*name));
    this->state.getInventoryStore().addFieldName(prop.name);
    return;
  }
  shape.named[i] = false;
  if (ObjectLiteralChecker::isLiteralKeys(keyType)) {
    computed->keyed = true;
    return;
  }
  shape.typed = false;
  if (keyType->isError()) {
    return;
  }
  ResultComputedKeyKnown key = ObjectLiteralChecker::computedKey(keyType);
  if (key.isErr()) {
    this->state.getDiagnostics().error(prop.key()->loc(), key.error().message);
  }
}

bool ObjectLiteralChecker::isLiteralKeys(const Type *keyType) {
  llvm::ArrayRef<const Type *> names =
      keyType->isUnion() ? keyType->members() : llvm::ArrayRef<const Type *>(keyType);
  for (const Type *n : names) {
    if (!n->isStringLiteral()) {
      return false;
    }
  }

  return true;
}

void ObjectLiteralChecker::walkUniqueNames(ObjectLitExpr *lit, ObjectLitShape &shape) {
  for (size_t i = 0; i < lit->props.size(); ++i) {
    ResultFieldNameIsFresh fresh = ObjectLiteralChecker::uniqueName(lit, i, shape.named);
    if (fresh.isErr()) {
      this->state.getDiagnostics().error(lit->props[i].loc, fresh.error().message);
      shape.typed = false;
    }
  }
}

Expr *ObjectLiteralChecker::pickUnionTarget(ObjectLitExpr *lit, const Type *expectedUnion,
                                            ObjectLitShape &shape) {
  const Type *member = this->literalTypes->literalMember(lit, expectedUnion);
  shape.target = ObjectLiteralChecker::isUsableTarget(member) ? member : nullptr;
  if (shape.target != nullptr) {
    return nullptr;
  }
  for (const Type *m : expectedUnion->members()) {
    if (m->isRecord()) {
      return this->recordLiteralChecker->checkRecordLit(lit, m);
    }
  }

  return nullptr;
}

void ObjectLiteralChecker::walkThisMethods(ObjectLitExpr *lit, const Type *target) {
  for (ObjectLitExpr::Property &prop : lit->props) {
    const Field *wanted = prop.method() ? target->field(prop.name) : nullptr;
    bool isBound = wanted != nullptr && wanted->type->nonOptional()->isFunction() &&
                   wanted->type->nonOptional()->hasThis();
    if (isBound) {
      prop.form = ValueProperty{};
    }
  }
}

bool ObjectLiteralChecker::isAllKeyed(const ObjectLitExpr *lit) {
  for (const ObjectLitExpr::Property &prop : lit->props) {
    if (!prop.keyed()) {
      return false;
    }
  }

  return true;
}

bool ObjectLiteralChecker::hasMethod(const ObjectLitExpr *lit) {
  for (const ObjectLitExpr::Property &prop : lit->props) {
    if (prop.method()) {
      return true;
    }
  }

  return false;
}

Expr *ObjectLiteralChecker::checkInferredKeys(ObjectLitExpr *lit, ObjectLitShape &shape) {
  std::vector<Field> fields;
  for (ObjectLitExpr::Property &prop : lit->props) {
    prop.value = this->exprChecker->checkExpr(prop.value);
    const Type *value = prop.value->type;
    if (value->isError()) {
      lit->type = Type::error();
      return lit;
    }
    ResultValueTypeIsKnown known = ObjectLiteralChecker::inferredValue(value);
    if (known.isErr()) {
      this->state.getDiagnostics().error(prop.value->loc(), known.error().message);
      lit->type = Type::error();
      return lit;
    }
    if (!this->keyedFieldsAgree(prop, value, fields)) {
      lit->type = Type::error();
      return lit;
    }
  }
  shape.target = Type::object(std::move(fields));
  shape.inferred = true;

  return nullptr;
}

bool ObjectLiteralChecker::keyedFieldsAgree(const ObjectLitExpr::Property &prop, const Type *value,
                                            std::vector<Field> &fields) {
  const Type *keyType = prop.key()->type;
  llvm::ArrayRef<const Type *> names =
      keyType->isUnion() ? keyType->members() : llvm::ArrayRef<const Type *>(keyType);
  for (const Type *n : names) {
    std::string name = ToUtf8::toUtf8(n->literalText());
    const Field *same = ObjectLiteralChecker::fieldNamed(fields, name);
    if (same == nullptr) {
      fields.push_back({name, Type::optional(value), false});
      continue;
    }
    ResultKeyedValuesAgree agree = ObjectLiteralChecker::sameKeyedType(name, *same, value);
    if (agree.isErr()) {
      this->state.getDiagnostics().error(prop.key()->loc(), agree.error().message);
      return false;
    }
  }

  return true;
}

const Field *ObjectLiteralChecker::fieldNamed(const std::vector<Field> &fields,
                                              llvm::StringRef name) {
  for (const Field &f : fields) {
    if (f.name == name) {
      return &f;
    }
  }

  return nullptr;
}

Expr *ObjectLiteralChecker::checkKeyedProperties(ObjectLitExpr *lit, const ObjectLitShape &shape) {
  for (ObjectLitExpr::Property &prop : lit->props) {
    if (!prop.keyed()) {
      continue;
    }
    Expr *failed = this->checkKeyedProperty(lit, prop, shape);
    if (failed != nullptr) {
      return failed;
    }
  }

  return nullptr;
}

Expr *ObjectLiteralChecker::checkKeyedProperty(ObjectLitExpr *lit, ObjectLitExpr::Property &prop,
                                               const ObjectLitShape &shape) {
  const Type *keyType = prop.key()->type;
  ResultKeyWithoutSpread placed = ObjectLiteralChecker::keyedPlacement(lit, keyType);
  if (placed.isErr()) {
    this->state.getDiagnostics().error(prop.key()->loc(), placed.error().message);
    return lit;
  }
  ResultKeyedObjectTyped object = ObjectLiteralChecker::keyedTarget(shape.target, keyType);
  if (object.isErr()) {
    this->state.getDiagnostics().error(prop.key()->loc(), object.error().message);
    prop.value = this->exprChecker->checkExpr(prop.value);
    lit->type = Type::error();
    return lit;
  }
  ResultKeyedFieldsAgree agree = ObjectLiteralChecker::keyedField(shape.target, keyType);
  if (agree.isErr()) {
    this->state.getDiagnostics().error(prop.key()->loc(), agree.error().message);
    prop.value = this->exprChecker->checkExpr(prop.value);
    lit->type = Type::error();
    return lit;
  }
  const Type *field = agree.value();
  std::string what = ErrorTextObjectLiteral::keyedName(keyType->name());
  Expr *value = shape.inferred ? prop.value : this->exprChecker->checkExpr(prop.value, field);
  prop.value = this->typeRelations->coerce(value, field, what);

  return nullptr;
}

std::vector<Field> ObjectLiteralChecker::checkNamedProperties(ObjectLitExpr *lit,
                                                              ObjectLitShape &shape) {
  std::vector<Field> fields;
  for (size_t i = 0; i < lit->props.size(); ++i) {
    ObjectLitExpr::Property &prop = lit->props[i];
    if (prop.keyed()) {
      continue;
    }
    if (!shape.named[i]) {
      prop.value = this->exprChecker->checkExpr(prop.value);
      continue;
    }
    auto *fe = dyn_cast<FunctionExpr>(prop.value);
    if (fe != nullptr && fe->fn->name.empty()) {
      fe->fn->name = prop.name;
    }
    if (shape.target != nullptr) {
      this->walkTargetField(prop, shape.target);
      continue;
    }
    fields.push_back(this->untargetedField(prop, shape));
  }

  return fields;
}

bool ObjectLiteralChecker::takePrechecked(const Expr *value) {
  bool isPrechecked = this->state.getExpressionMarks().isPrechecked(value);
  this->state.getExpressionMarks().forgetPrechecked(value);

  return isPrechecked;
}

void ObjectLiteralChecker::walkTargetField(ObjectLitExpr::Property &prop, const Type *target) {
  std::string what = ErrorTextObjectLiteral::fieldName(prop.name);
  ResultTargetFieldFound found = ObjectLiteralChecker::targetField(target, prop.name);
  bool isPrechecked = this->takePrechecked(prop.value);
  if (found.isErr()) {
    if (!isPrechecked) {
      prop.value = this->exprChecker->checkExpr(prop.value);
    }
    if (found.error().code != ObjectLiteralCheckerTargetFieldErrorCode::OVERLOAD_NAME) {
      this->state.getDiagnostics().error(prop.loc, found.error().message);
    }
    return;
  }
  const Field *field = found.value();
  Expr *value = isPrechecked ? prop.value : this->exprChecker->checkExpr(prop.value, field->type);
  prop.value = this->typeRelations->coerce(value, field->type, what);
}

Field ObjectLiteralChecker::untargetedField(ObjectLitExpr::Property &prop, ObjectLitShape &shape) {
  std::string what = ErrorTextObjectLiteral::fieldName(prop.name);
  bool isPrechecked = this->takePrechecked(prop.value);
  if (!isPrechecked) {
    prop.value = this->exprChecker->checkExpr(prop.value);
  }
  const Type *type = prop.value->type;
  if (type->isError()) {
    shape.typed = false;
    return Field{prop.name.str(), type, false};
  }
  ResultUsableFieldValue usable = ObjectLiteralChecker::valueTyped(type, what, prop.name);
  if (usable.isErr()) {
    this->state.getDiagnostics().error(prop.value->loc(), usable.error().message);
    shape.typed = false;
  }

  return Field{prop.name.str(), type, false};
}

Expr *ObjectLiteralChecker::finishObjectLit(ObjectLitExpr *lit, const ObjectLitShape &shape,
                                            std::vector<Field> fields) {
  const Type *target = shape.target;
  if (target == nullptr) {
    lit->type = shape.typed ? Type::object(std::move(fields)) : Type::error();
    return lit;
  }
  for (const Field &f : target->fields()) {
    ResultRequiredFieldSet given = ObjectLiteralChecker::fieldGiven(lit, f, target);
    if (given.isErr()) {
      this->state.getDiagnostics().error(lit->loc(), given.error().message);
    }
  }
  for (const Overload &o : target->overloads()) {
    std::string message = ErrorTextObjectLiteral::overloads(target->name(), o.name);
    this->state.getDiagnostics().error(lit->loc(), message);
  }
  lit->type = target;

  return lit;
}

ObjectLiteralChecker::ResultDefinedSpreading ObjectLiteralChecker::spreadDefined(const Type *type) {
  if (type->isOptional() && type->inner()->isObject() && !type->inner()->isTuple()) {
    std::string message = ErrorTextSpreadLiteral::undefined();

    return err(ObjectLiteralCheckerSpreadDefinedError{
        .code = ObjectLiteralCheckerSpreadDefinedErrorCode::MAY_BE_UNDEFINED,
        .message = message,
    });
  }

  return ok(type);
}

ObjectLiteralChecker::ResultSpreadIsAnObject ObjectLiteralChecker::spreadObject(const Type *type) {
  bool isObject = type->isObject() && !type->isTuple() && !type->isRegExp() && !type->isMatch() &&
                  !type->isDate() && !type->isIntl() && !type->isBytes() && !type->isCrypto();
  if (!isObject) {
    bool hasIndices =
        type->nonOptional()->isArray() || type->nonOptional()->isTuple() || type->isString();
    std::string message = ErrorTextSpreadLiteral::notObject(type->name(), hasIndices);

    return err(ObjectLiteralCheckerSpreadObjectError{
        .code = ObjectLiteralCheckerSpreadObjectErrorCode::NOT_AN_OBJECT,
        .message = message,
    });
  }

  return ok(type);
}

ObjectLiteralChecker::ResultPlainSpreadClass
ObjectLiteralChecker::spreadFields(const ClassDecl *cls) {
  if (ObjectLiteralChecker::hasMethodFields(cls)) {
    std::string message = ErrorTextSpreadLiteral::methods();

    return err(ObjectLiteralCheckerSpreadFieldsError{
        .code = ObjectLiteralCheckerSpreadFieldsErrorCode::METHOD_FIELDS,
        .message = message,
    });
  }

  return ok(cls);
}

ObjectLiteralChecker::ResultSpreadFieldJoins
ObjectLiteralChecker::spreadJoin(const Field &spread, const Field &field, const Type *type) {
  const Type *base = TypeRelations::unify(field.type->nonOptional(), spread.type->inner());
  if (!base || base->isError()) {
    std::string message = ErrorTextSpreadLiteral::twoTypes(spread.name, field.type->name(),
                                                           spread.type->name(), type->name());

    return err(ObjectLiteralCheckerSpreadJoinError{
        .code = ObjectLiteralCheckerSpreadJoinErrorCode::TYPES_DIFFER,
        .message = message,
    });
  }

  return ok(base);
}

Expr *ObjectLiteralChecker::checkSpreadLit(ObjectLitExpr *lit, const Type *target,
                                           llvm::ArrayRef<bool> named, bool typed) {
  SpreadFields collected{.target = target, .fields = {}, .records = false, .typed = typed};
  for (size_t i = 0; i < lit->props.size(); ++i) {
    ObjectLitExpr::Property &prop = lit->props[i];
    if (prop.spread()) {
      this->walkSpreadProp(prop, collected);
    } else {
      this->walkNamedProp(prop, named[i], collected);
    }
  }
  if (collected.records && collected.typed) {
    return this->recordLiteralChecker->recordFromSpreads(lit);
  }
  if (!collected.typed) {
    lit->type = Type::error();
    return lit;
  }
  const Type *result = ObjectLiteralChecker::spreadResult(collected);
  this->convertSpreadProps(lit, result);
  lit->type = result;

  return lit;
}

Field *ObjectLiteralChecker::findSlot(std::vector<Field> &fields, llvm::StringRef name) {
  for (Field &f : fields) {
    if (f.name == name) {
      return &f;
    }
  }

  return nullptr;
}

void ObjectLiteralChecker::walkNamedProp(ObjectLitExpr::Property &prop, bool isNamed,
                                         SpreadFields &collected) {
  if (!isNamed) {
    prop.value = this->exprChecker->checkExpr(prop.value);
    return;
  }
  auto *fe = dyn_cast<FunctionExpr>(prop.value);
  if (fe != nullptr && fe->fn->name.empty()) {
    fe->fn->name = prop.name;
  }
  const Field *wanted = collected.target != nullptr ? collected.target->field(prop.name) : nullptr;
  prop.value = this->exprChecker->checkExpr(prop.value, wanted != nullptr ? wanted->type : nullptr);
  const Type *type = prop.value->type;
  Field *field = ObjectLiteralChecker::findSlot(collected.fields, prop.name);
  std::string what = ErrorTextObjectLiteral::fieldName(prop.name);
  if (type->isError()) {
    collected.typed = false;
    return;
  }
  bool isFilled = type->isUndefined() && field != nullptr;
  ResultUsableFieldValue usableValue =
      isFilled ? ResultUsableFieldValue(ok(type))
               : ObjectLiteralChecker::valueTyped(type, what, prop.name);
  if (usableValue.isErr()) {
    this->state.getDiagnostics().error(prop.value->loc(), usableValue.error().message);
    collected.typed = false;
    return;
  }
  if (isFilled) {
    type = Type::optional(field->type);
  }
  if (field != nullptr) {
    field->type = type;
  } else {
    collected.fields.push_back({prop.name.str(), type, false});
  }
}

void ObjectLiteralChecker::walkSpreadProp(ObjectLitExpr::Property &prop, SpreadFields &collected) {
  prop.value = this->exprChecker->checkExpr(prop.value);
  const Type *type = prop.value->type;
  if (type->isError()) {
    collected.typed = false;
    return;
  }
  if (type->isRecord() && collected.target == nullptr) {
    collected.records = true;
    return;
  }
  if (!this->spreadTypeFits(prop, type)) {
    collected.typed = false;
    return;
  }
  ObjectLiteralChecker::takeOmitKeys(prop);
  if (!this->spreadClassFits(prop, type)) {
    collected.typed = false;
    return;
  }
  this->mergeSpreadFields(prop, type, collected);
}

bool ObjectLiteralChecker::spreadTypeFits(const ObjectLitExpr::Property &prop, const Type *type) {
  ResultDefinedSpreading defined = ObjectLiteralChecker::spreadDefined(type);
  if (defined.isErr()) {
    this->state.getDiagnostics().error(prop.value->loc(), defined.error().message);
    return false;
  }
  ResultSpreadIsAnObject object = ObjectLiteralChecker::spreadObject(type);
  if (object.isErr()) {
    this->state.getDiagnostics().error(prop.value->loc(), object.error().message);
    return false;
  }

  return true;
}

void ObjectLiteralChecker::takeOmitKeys(ObjectLitExpr::Property &prop) {
  SpreadProperty *spread = prop.form.as<SpreadProperty>();
  for (const IndexExpr *read : spread->omitKeys) {
    if (!read->field().empty()) {
      spread->omit.push_back(read->field());
    }
  }
  spread->omitKeys.clear();
}

bool ObjectLiteralChecker::spreadClassFits(const ObjectLitExpr::Property &prop, const Type *type) {
  ClassDecl *cls = ClassDeclarer::classOf(type);
  if (cls == nullptr) {
    return true;
  }
  this->classDeclarer->completeClass(cls);
  ResultPlainSpreadClass plain = ObjectLiteralChecker::spreadFields(cls);
  if (plain.isErr()) {
    this->state.getDiagnostics().error(prop.value->loc(), plain.error().message);
    return false;
  }

  return true;
}

void ObjectLiteralChecker::mergeSpreadFields(const ObjectLitExpr::Property &prop, const Type *type,
                                             SpreadFields &collected) {
  for (const Field &f : type->fields()) {
    if (llvm::is_contained(prop.omit(), f.name)) {
      continue;
    }
    Field *field = ObjectLiteralChecker::findSlot(collected.fields, f.name);
    if (field == nullptr) {
      collected.fields.push_back({f.name, f.type, false});
      continue;
    }
    if (!f.type->isOptional()) {
      field->type = f.type;
      continue;
    }
    ResultSpreadFieldJoins joined = ObjectLiteralChecker::spreadJoin(f, *field, type);
    if (joined.isErr()) {
      this->state.getDiagnostics().error(prop.loc, joined.error().message);
      collected.typed = false;
      continue;
    }
    const Type *base = joined.value();
    field->type = field->type->isOptional() ? Type::optional(base) : base;
  }
}

const Type *ObjectLiteralChecker::spreadResult(const SpreadFields &collected) {
  const Type *result = Type::object(collected.fields);
  if (collected.target == nullptr) {
    return result;
  }
  bool isFitting = ObjectLiteralChecker::fieldsFitTarget(collected.fields, collected.target) &&
                   ObjectLiteralChecker::targetFilled(collected.target, collected.fields);

  return isFitting ? collected.target : result;
}

bool ObjectLiteralChecker::fieldsFitTarget(const std::vector<Field> &fields, const Type *target) {
  for (const Field &f : fields) {
    const Field *t = target->field(f.name);
    if (t == nullptr || !TypeRelations::isImplicitlyConvertible(f.type, t->type)) {
      return false;
    }
  }

  return true;
}

bool ObjectLiteralChecker::targetFilled(const Type *target, const std::vector<Field> &fields) {
  for (const Field &t : target->fields()) {
    if (!t.type->isOptional() && ObjectLiteralChecker::fieldNamed(fields, t.name) == nullptr) {
      return false;
    }
  }

  return true;
}

void ObjectLiteralChecker::convertSpreadProps(ObjectLitExpr *lit, const Type *result) {
  for (size_t i = 0; i < lit->props.size(); ++i) {
    ObjectLitExpr::Property &prop = lit->props[i];
    if (prop.spread()) {
      ObjectLiteralChecker::markReplacedFields(lit, i);
      continue;
    }
    if (ObjectLiteralChecker::isReplacedLater(lit, i, prop.name)) {
      prop.replaced = true;
      continue;
    }
    prop.value = this->typeRelations->convert(prop.value, result->field(prop.name)->type);
  }
}

void ObjectLiteralChecker::markReplacedFields(ObjectLitExpr *lit, size_t i) {
  ObjectLitExpr::Property &prop = lit->props[i];
  SpreadProperty *spread = prop.form.as<SpreadProperty>();
  for (const Field &f : prop.value->type->fields()) {
    bool isOmitted = llvm::is_contained(spread->omit, f.name);
    if (!isOmitted && ObjectLiteralChecker::isReplacedLater(lit, i, f.name)) {
      spread->replacedFields.push_back(f.name);
    }
  }
}

bool ObjectLiteralChecker::isReplacedLater(const ObjectLitExpr *lit, size_t i,
                                           llvm::StringRef name) {
  for (size_t j = i + 1; j < lit->props.size(); ++j) {
    const ObjectLitExpr::Property &later = lit->props[j];
    if (!later.spread()) {
      if (later.name == name) {
        return true;
      }
      continue;
    }
    if (llvm::is_contained(later.omit(), name)) {
      continue;
    }
    const Field *f = later.value->type->field(name);
    if (f != nullptr && !f->type->isOptional()) {
      return true;
    }
  }

  return false;
}

}
