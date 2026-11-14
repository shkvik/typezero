#include "compiler/semantic/code/literals/object_literal_properties/object_literal_properties.h"
#include "compiler/program/library/builtin_names/prelude_type_table/prelude_type_table.h"
#include "compiler/program/to_utf8/to_utf8.h"
#include "llvm/ADT/STLExtras.h"

namespace tz {

ObjectLiteralProperties::ObjectLiteralProperties(SemanticState &owner,
                                                 LiteralRules &sharedLiteralRules)
    : state(owner),
      literalRules(sharedLiteralRules) {}

void ObjectLiteralProperties::nextInferredKey(const NextInferredKey &task) {
  auto draft = task.draft;
  ObjectLitExpr *literal = draft.literal;
  if (draft.index == literal->props.size()) {
    draft.target = Type::object(std::move(draft.fields));
    draft.fields = {};
    draft.isInferred = true;
    draft.index = 0;
    return this->nextKeyedProperty(NextKeyedProperty{.draft = std::move(draft)});
  }
  Expr **value = &literal->props[draft.index].value;
  this->state.walk.tasks.pushTask(AfterInferredKey{.draft = std::move(draft)});

  return this->state.walk.tasks.pushTask(CheckExprSlot{.slot = value, .expected = nullptr});
}

void ObjectLiteralProperties::afterInferredKey(const AfterInferredKey &task) {
  auto draft = task.draft;
  ObjectLitExpr *literal = draft.literal;
  ObjectLitExpr::Property &property = literal->props[draft.index];
  const Type *value = property.value->type;
  bool isFailed = value->isError();
  if (!isFailed) {
    auto known = this->checkInferredValue(value);
    if (known.isErr()) {
      this->state.diagnostics.error(property.value->loc(), known.error().message);
    }
    isFailed = known.isErr() || this->reportKeyedFields(property, value, draft.fields);
  }
  if (isFailed) {
    literal->type = Type::error();
    return this->state.walk.tasks.pushChecked(literal);
  }
  draft.index += 1;

  return this->nextInferredKey(NextInferredKey{.draft = std::move(draft)});
}

void ObjectLiteralProperties::nextKeyedProperty(const NextKeyedProperty &task) {
  auto draft = task.draft;
  ObjectLitExpr *literal = draft.literal;
  while (draft.index < literal->props.size() && !literal->props[draft.index].keyed()) {
    draft.index += 1;
  }
  if (draft.index == literal->props.size()) {
    return this->afterKeyedProperties(std::move(draft));
  }
  ObjectLitExpr::Property &property = literal->props[draft.index];
  const Type *keyType = property.key()->type;
  auto &tasks = this->state.walk.tasks;
  auto placed = this->checkKeyedPlacement(literal, keyType);
  if (placed.isErr()) {
    this->state.diagnostics.error(property.key()->loc(), placed.error().message);
    literal->type = Type::error();
    return tasks.pushChecked(literal);
  }
  auto object = this->checkKeyedTarget(draft.target, keyType);
  if (object.isErr()) {
    return this->failKeyedProperty(literal, property, object.error().message);
  }
  auto agree = this->checkKeyedField(draft.target, keyType);
  if (agree.isErr()) {
    return this->failKeyedProperty(literal, property, agree.error().message);
  }
  const Type *field = agree.value();
  auto what = ErrorTextObjectLiteral::keyedName(keyType->name());
  bool isInferred = draft.isInferred;
  draft.index += 1;
  tasks.pushTask(NextKeyedProperty{.draft = std::move(draft)});
  tasks.pushTask(CoerceSlot{.slot = &property.value, .to = field, .what = what});
  if (!isInferred) {
    return tasks.pushTask(CheckExprSlot{.slot = &property.value, .expected = field});
  }
}

void ObjectLiteralProperties::failKeyedProperty(ObjectLitExpr *literal,
                                                ObjectLitExpr::Property &property,
                                                const std::string &message) {
  this->state.diagnostics.error(property.key()->loc(), message);
  literal->type = Type::error();
  this->state.walk.tasks.pushChecked(literal);

  return this->state.walk.tasks.pushTask(
      CheckExprSlot{.slot = &property.value, .expected = nullptr});
}

void ObjectLiteralProperties::afterKeyedProperties(ObjectLitDraft draft) {
  ObjectLitExpr *literal = draft.literal;
  if (this->hasMethod(literal)) {
    return this->startMethodLiteral(literal, draft.target);
  }
  if (literal->hasSpread()) {
    return this->startSpreadLit(literal, draft.target, draft.named, draft.isTyped);
  }
  draft.index = 0;

  return this->nextNamedProperty(NextNamedProperty{.draft = std::move(draft)});
}

void ObjectLiteralProperties::nextNamedProperty(const NextNamedProperty &task) {
  auto draft = task.draft;
  ObjectLitExpr *literal = draft.literal;
  while (draft.index < literal->props.size() && literal->props[draft.index].keyed()) {
    draft.index += 1;
  }
  if (draft.index == literal->props.size()) {
    return this->finishObjectLit(draft);
  }
  ObjectLitExpr::Property &property = literal->props[draft.index];
  bool isNamed = draft.named[draft.index];
  auto &tasks = this->state.walk.tasks;
  if (!isNamed) {
    draft.index += 1;
    tasks.pushTask(NextNamedProperty{.draft = std::move(draft)});
    return tasks.pushTask(CheckExprSlot{.slot = &property.value, .expected = nullptr});
  }
  this->literalRules.nameFunctionValue(property);
  bool isPrechecked = this->takePrechecked(property.value);
  if (draft.target == nullptr) {
    tasks.pushTask(AfterUntargetedField{.draft = std::move(draft)});
    if (!isPrechecked) {
      return tasks.pushTask(CheckExprSlot{.slot = &property.value, .expected = nullptr});
    }
    return;
  }
  auto found = this->checkTargetField(draft.target, property.name);
  draft.index += 1;
  tasks.pushTask(NextNamedProperty{.draft = std::move(draft)});
  if (found.isErr()) {
    if (found.error().code != CodeLiteralsRegistryTargetFieldErrorCode::OVERLOAD_NAME) {
      tasks.pushTask(ReportError{.loc = property.loc, .message = found.error().message});
    }
    if (!isPrechecked) {
      return tasks.pushTask(CheckExprSlot{.slot = &property.value, .expected = nullptr});
    }
    return;
  }
  const Field *field = found.value();
  auto what = ErrorTextObjectLiteral::fieldName(property.name);
  tasks.pushTask(CoerceSlot{.slot = &property.value, .to = field->type, .what = what});
  if (!isPrechecked) {
    return tasks.pushTask(CheckExprSlot{.slot = &property.value, .expected = field->type});
  }
}

void ObjectLiteralProperties::afterUntargetedField(const AfterUntargetedField &task) {
  auto draft = task.draft;
  ObjectLitExpr::Property &property = draft.literal->props[draft.index];
  const Type *type = property.value->type;
  bool isUsable = !type->isError() && !this->literalRules.reportUnusableValue(property, type);
  if (!isUsable) {
    draft.isTyped = false;
  }
  draft.fields.push_back(Field{property.name.str(), type, false});
  draft.index += 1;

  return this->nextNamedProperty(NextNamedProperty{.draft = std::move(draft)});
}

void ObjectLiteralProperties::finishObjectLit(const ObjectLitDraft &draft) {
  ObjectLitExpr *literal = draft.literal;
  const Type *target = draft.target;
  this->state.walk.tasks.pushChecked(literal);
  if (target == nullptr) {
    literal->type = draft.isTyped ? Type::object(draft.fields) : Type::error();
    return;
  }
  for (const Field &field : target->fields()) {
    auto given = this->checkFieldGiven(literal, field, target);
    if (given.isErr()) {
      this->state.diagnostics.error(literal->loc(), given.error().message);
    }
  }
  for (const Overload &overload : target->overloads()) {
    auto message = ErrorTextObjectLiteral::overloads(target->name(), overload.name);
    this->state.diagnostics.error(literal->loc(), message);
  }
  literal->type = target;
}

void ObjectLiteralProperties::startSpreadLit(ObjectLitExpr *literal, const Type *target,
                                             const std::vector<bool> &named, bool isTyped) {
  auto draft = SpreadDraft{
      .literal = literal,
      .target = target,
      .named = named,
      .fields = {},
      .hasRecords = false,
      .isTyped = isTyped,
      .index = 0,
  };

  return this->state.walk.tasks.pushTask(NextSpreadProperty{.draft = std::move(draft)});
}

void ObjectLiteralProperties::startMethodLiteral(ObjectLitExpr *literal, const Type *target) {
  literal->type = Type::error();
  const Type *wanted = target != nullptr && target->isAnyObject() ? nullptr : target;
  auto spreadless = this->checkNoSpread(literal);
  if (spreadless.isErr()) {
    this->state.diagnostics.error(literal->loc(), spreadless.error().message);
    this->state.walk.tasks.pushChecked(literal);
    for (ObjectLitExpr::Property &property : llvm::reverse(literal->props)) {
      this->state.walk.tasks.pushTask(CheckExprSlot{.slot = &property.value, .expected = nullptr});
    }
    return;
  }
  auto *type = this->makeLiteralClass(literal);
  auto draft = MethodLiteralDraft{
      .literal = literal,
      .wanted = wanted,
      .type = type,
      .fields = {},
      .hasFieldsTyped = true,
      .index = 0,
  };

  return this->state.walk.tasks.pushTask(NextLiteralMember{.draft = std::move(draft)});
}

const Type *ObjectLiteralProperties::keyedFieldType(const Type *type, const Type *keyType) const {
  if (type->isTuple() || !type->isObject()) {
    return nullptr;
  }
  const ClassDecl *classDecl = type->isClass() ? this->literalRules.classOf(type) : nullptr;
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

bool ObjectLiteralProperties::isPublicField(const ClassDecl *classDecl,
                                            const std::string &name) const {
  const auto *field = classDecl->findField(name);

  return field != nullptr && !field->isStatic() && field->access == Access::Public;
}

ObjectLiteralProperties::ResultValueTypeIsKnown
ObjectLiteralProperties::checkInferredValue(const Type *value) const {
  if (value->isVoid() || value->isUndefined()) {
    auto message = ErrorTextObjectLiteral::inferFrom(value->name());

    return err(CodeLiteralsRegistryInferredValueError{
        .code = CodeLiteralsRegistryInferredValueErrorCode::CANNOT_INFER,
        .message = message,
    });
  }

  return ok(value);
}

ObjectLiteralProperties::ResultKeyedValuesAgree
ObjectLiteralProperties::checkSameKeyedType(llvm::StringRef name, const Field &same,
                                            const Type *value) const {
  if (same.type != Type::optional(value)) {
    auto message = ErrorTextObjectLiteral::keyTypes(name);

    return err(CodeLiteralsRegistrySameKeyedTypeError{
        .code = CodeLiteralsRegistrySameKeyedTypeErrorCode::FIELD_TYPES_DIFFER,
        .message = message,
    });
  }

  return ok(value);
}

ObjectLiteralProperties::ResultKeyedObjectTyped
ObjectLiteralProperties::checkKeyedTarget(const Type *target, const Type *keyType) const {
  if (target == nullptr) {
    auto message = ErrorTextObjectLiteral::keyNoType(keyType->name());

    return err(CodeLiteralsRegistryKeyedTargetError{
        .code = CodeLiteralsRegistryKeyedTargetErrorCode::NO_OBJECT_TYPE,
        .message = message,
    });
  }

  return ok(target);
}

ObjectLiteralProperties::ResultKeyedFieldsAgree
ObjectLiteralProperties::checkKeyedField(const Type *target, const Type *keyType) const {
  const auto *field = this->keyedFieldType(target, keyType);
  if (field == nullptr) {
    auto message = ErrorTextObjectLiteral::keyFields(keyType->name(), target->name());

    return err(CodeLiteralsRegistryKeyedFieldError{
        .code = CodeLiteralsRegistryKeyedFieldErrorCode::FIELDS_DIFFER,
        .message = message,
    });
  }
  if (!field->isOptional()) {
    auto message = ErrorTextObjectLiteral::keyedOpt(keyType->name(), field->name());

    return err(CodeLiteralsRegistryKeyedFieldError{
        .code = CodeLiteralsRegistryKeyedFieldErrorCode::NOT_OPTIONAL,
        .message = message,
    });
  }

  return ok(field);
}

ObjectLiteralProperties::ResultKeyWithoutSpread
ObjectLiteralProperties::checkKeyedPlacement(const ObjectLitExpr *literal,
                                             const Type *keyType) const {
  bool hasMethods = this->hasMethod(literal);
  if (literal->hasSpread() || hasMethods) {
    auto message = ErrorTextObjectLiteral::keySpread(keyType->name());

    return err(CodeLiteralsRegistryKeyedPlacementError{
        .code = CodeLiteralsRegistryKeyedPlacementErrorCode::KEY_WITH_SPREAD,
        .message = message,
    });
  }

  return ok(keyType);
}

ObjectLiteralProperties::ResultTargetFieldFound
ObjectLiteralProperties::checkTargetField(const Type *target, llvm::StringRef name) const {
  const auto *field = target->field(name);
  if (field == nullptr && target->overload(name) != nullptr) {
    return err(CodeLiteralsRegistryTargetFieldError{
        .code = CodeLiteralsRegistryTargetFieldErrorCode::OVERLOAD_NAME,
        .message = "",
    });
  }
  if (field == nullptr) {
    auto streamError = PreludeTypeTable::getName(PreludeType::STREAM_ERROR);
    bool isStream = target->name() == streamError;
    auto message = ErrorTextObjectLiteral::noField(target->name(), name, isStream);

    return err(CodeLiteralsRegistryTargetFieldError{
        .code = CodeLiteralsRegistryTargetFieldErrorCode::NO_SUCH_FIELD,
        .message = message,
    });
  }

  return ok(field);
}

ObjectLiteralProperties::ResultRequiredFieldSet
ObjectLiteralProperties::checkFieldGiven(const ObjectLitExpr *literal, const Field &field,
                                         const Type *target) const {
  bool isGiven = this->hasPropertyNamed(literal, field.name);
  if (!isGiven && !field.type->isOptional()) {
    auto message = ErrorTextObjectLiteral::missing(field.name, target->name());

    return err(CodeLiteralsRegistryFieldGivenError{
        .code = CodeLiteralsRegistryFieldGivenErrorCode::MISSING_FIELD,
        .message = message,
    });
  }

  return ok(&field);
}

bool ObjectLiteralProperties::hasPropertyNamed(const ObjectLitExpr *literal,
                                               llvm::StringRef name) const {
  for (const ObjectLitExpr::Property &property : literal->props) {
    if (property.name == name) {
      return true;
    }
  }

  return false;
}

bool ObjectLiteralProperties::hasMethod(const ObjectLitExpr *literal) const {
  for (const ObjectLitExpr::Property &property : literal->props) {
    if (property.method()) {
      return true;
    }
  }

  return false;
}

bool ObjectLiteralProperties::reportKeyedFields(const ObjectLitExpr::Property &property,
                                                const Type *value, std::vector<Field> &fields) {
  const Type *keyType = property.key()->type;
  llvm::ArrayRef<const Type *> names =
      keyType->isUnion() ? keyType->members() : llvm::ArrayRef<const Type *>(keyType);
  for (const Type *nameType : names) {
    auto name = ToUtf8::toUtf8(nameType->literalText());
    const auto *same = this->literalRules.fieldNamed(fields, name);
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

bool ObjectLiteralProperties::reportKeyedClash(const ObjectLitExpr::Property &property,
                                               llvm::StringRef name, const Field &same,
                                               const Type *value) {
  auto agree = this->checkSameKeyedType(name, same, value);
  if (agree.isOk()) {
    return false;
  }
  this->state.diagnostics.error(property.key()->loc(), agree.error().message);

  return true;
}

bool ObjectLiteralProperties::takePrechecked(const Expr *value) {
  bool isPrechecked = this->state.code.literals.isPrechecked(value);
  this->state.code.literals.forgetPrechecked(value);

  return isPrechecked;
}

Type *ObjectLiteralProperties::makeLiteralClass(ObjectLitExpr *literal) {
  auto *classDecl = this->state.ast.make<ClassDecl>(literal->loc(), "");
  classDecl->syntax = LiteralClass{.literal = literal};
  classDecl->id = this->state.declaration.classes.takeClassId();
  literal->cls = classDecl;
  auto *type = Type::classType("", classDecl);
  classDecl->type = type;
  classDecl->stage = ClassStage::RESOLVED;
  classDecl->declared = true;
  this->state.declaration.classes.addClass(classDecl);

  return type;
}

ObjectLiteralProperties::ResultSpreadAbsent
ObjectLiteralProperties::checkNoSpread(const ObjectLitExpr *literal) const {
  if (literal->hasSpread()) {
    auto message = ErrorTextLiteralSpread::spread();

    return err(CodeLiteralsRegistryNoSpreadError{
        .code = CodeLiteralsRegistryNoSpreadErrorCode::SPREAD_IN_LITERAL,
        .message = message,
    });
  }

  return ok(literal);
}

}
