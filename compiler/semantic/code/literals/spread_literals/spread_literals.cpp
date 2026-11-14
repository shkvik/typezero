#include "compiler/semantic/code/literals/spread_literals/spread_literals.h"
#include "llvm/ADT/STLExtras.h"

using llvm::cast;
using llvm::dyn_cast;

namespace tz {

SpreadLiterals::SpreadLiterals(SemanticState &owner, LiteralRules &sharedLiteralRules)
    : state(owner),
      literalRules(sharedLiteralRules) {}

void SpreadLiterals::nextRecordProperty(const NextRecordProperty &task) {
  auto draft = task.draft;
  ObjectLitExpr *literal = draft.literal;
  auto &tasks = this->state.walk.tasks;
  if (draft.index == literal->props.size()) {
    literal->type = draft.hasFailed ? Type::error() : draft.record;
    return tasks.pushChecked(literal);
  }
  ObjectLitExpr::Property &property = literal->props[draft.index];
  auto plain = this->checkPlainProperty(property);
  if (plain.isErr()) {
    this->state.diagnostics.error(property.loc, plain.error().message);
    draft.hasFailed = true;
  }
  if (property.defaulted()) {
    property.value = cast<AssignExpr>(property.value)->value;
    property.form = ValueProperty{};
  }
  draft.index += 1;
  if (property.method()) {
    tasks.pushTask(NextRecordProperty{.draft = draft});
    return tasks.pushTask(CheckExprSlot{.slot = &property.value, .expected = nullptr});
  }
  if (property.spread()) {
    tasks.pushTask(AfterRecordSpread{.draft = draft});
    return tasks.pushTask(CheckExprSlot{.slot = &property.value, .expected = draft.record});
  }
  const Type *value = draft.record->valueType();
  auto *functionExpr = dyn_cast<FunctionExpr>(property.value);
  if (functionExpr != nullptr && functionExpr->fn->name.empty()) {
    functionExpr->fn->name = property.name;
  }
  std::string what = property.key() ? ErrorTextRecordProperty::valueLabel()
                                    : ErrorTextRecordProperty::keyLabel(property.name);
  tasks.pushTask(NextRecordProperty{.draft = draft});
  tasks.pushTask(CoerceSlot{.slot = &property.value, .to = value, .what = what});
  tasks.pushTask(CheckExprSlot{.slot = &property.value, .expected = value});
  ComputedProperty *computed = property.form.as<ComputedProperty>();
  if (computed != nullptr) {
    return tasks.pushTask(CheckRecordKeySlot{.slot = &computed->key});
  }
}

void SpreadLiterals::afterRecordSpread(const AfterRecordSpread &task) {
  auto draft = task.draft;
  ObjectLitExpr::Property &property = draft.literal->props[draft.index - 1];
  bool isFailed = property.value->type->isError() ||
                  this->reportSpreadIntoRecord(property, draft.record->valueType());
  draft.hasFailed = draft.hasFailed || isFailed;

  return this->nextRecordProperty(NextRecordProperty{.draft = draft});
}

void SpreadLiterals::nextSpreadProperty(const NextSpreadProperty &task) {
  auto draft = task.draft;
  ObjectLitExpr *literal = draft.literal;
  auto &tasks = this->state.walk.tasks;
  if (draft.index == literal->props.size()) {
    return this->finishSpreadLit(draft);
  }
  ObjectLitExpr::Property &property = literal->props[draft.index];
  if (property.spread()) {
    tasks.pushTask(AfterSpreadProp{.draft = std::move(draft)});
    return tasks.pushTask(CheckExprSlot{.slot = &property.value, .expected = nullptr});
  }
  if (!draft.named[draft.index]) {
    draft.index += 1;
    tasks.pushTask(NextSpreadProperty{.draft = std::move(draft)});
    return tasks.pushTask(CheckExprSlot{.slot = &property.value, .expected = nullptr});
  }
  this->literalRules.nameFunctionValue(property);
  const Field *wanted = draft.target != nullptr ? draft.target->field(property.name) : nullptr;
  const Type *hint = wanted != nullptr ? wanted->type : nullptr;
  tasks.pushTask(AfterNamedProp{.draft = std::move(draft)});

  return tasks.pushTask(CheckExprSlot{.slot = &property.value, .expected = hint});
}

void SpreadLiterals::afterNamedProp(const AfterNamedProp &task) {
  auto draft = task.draft;
  ObjectLitExpr::Property &property = draft.literal->props[draft.index];
  draft.index += 1;
  const Type *type = property.value->type;
  auto *field = this->findSlot(draft.fields, property.name);
  bool isFilled = type->isUndefined() && field != nullptr;
  if (type->isError() || (!isFilled && this->literalRules.reportUnusableValue(property, type))) {
    draft.isTyped = false;
  } else if (field != nullptr) {
    field->type = isFilled ? Type::optional(field->type) : type;
  } else {
    draft.fields.push_back({property.name.str(), type, false});
  }

  return this->nextSpreadProperty(NextSpreadProperty{.draft = std::move(draft)});
}

void SpreadLiterals::afterSpreadProp(const AfterSpreadProp &task) {
  auto draft = task.draft;
  ObjectLitExpr::Property &property = draft.literal->props[draft.index];
  draft.index += 1;
  const Type *type = property.value->type;
  if (type->isError()) {
    draft.isTyped = false;
  } else if (type->isRecord() && draft.target == nullptr) {
    draft.hasRecords = true;
  } else if (this->reportSpreadType(property, type)) {
    draft.isTyped = false;
  } else {
    this->takeOmitKeys(property);
    bool isClassFailed = this->reportSpreadClass(property, type);
    if (isClassFailed) {
      draft.isTyped = false;
    } else {
      this->mergeSpreadFields(property, type, draft);
    }
  }

  return this->nextSpreadProperty(NextSpreadProperty{.draft = std::move(draft)});
}

void SpreadLiterals::finishSpreadLit(const SpreadDraft &draft) {
  ObjectLitExpr *literal = draft.literal;
  if (draft.hasRecords && draft.isTyped) {
    return this->recordFromSpreads(literal);
  }
  this->state.walk.tasks.pushChecked(literal);
  if (!draft.isTyped) {
    literal->type = Type::error();
    return;
  }
  const auto *result = this->spreadResult(draft);
  literal->type = result;
  for (size_t index = literal->props.size(); index > 0; --index) {
    this->convertSpreadProp(literal, index - 1, result);
  }
}

void SpreadLiterals::convertSpreadProp(ObjectLitExpr *literal, size_t index, const Type *result) {
  ObjectLitExpr::Property &property = literal->props[index];
  if (property.spread()) {
    return this->markReplacedFields(literal, index);
  }
  if (this->isReplacedLater(literal, index, property.name)) {
    property.replaced = true;
    return;
  }
  const Type *fieldType = result->field(property.name)->type;

  return this->state.walk.tasks.pushTask(ConvertSlot{.slot = &property.value, .to = fieldType});
}

void SpreadLiterals::recordFromSpreads(ObjectLitExpr *literal) {
  auto candidates = this->valueCandidates(literal);
  auto joined = RecordValue{.value = nullptr, .isTyped = true};
  for (const ValueCandidate &candidate : candidates) {
    joined = this->joinValue(joined, candidate);
  }
  this->state.walk.tasks.pushChecked(literal);
  if (!joined.isTyped || joined.value == nullptr) {
    literal->type = Type::error();
    return;
  }
  const Type *value = joined.value;
  auto known = this->checkValueKnown(value);
  if (known.isErr()) {
    this->state.diagnostics.error(literal->loc(), known.error().message);
    literal->type = Type::error();
    return;
  }
  bool hasFailed = false;
  for (ObjectLitExpr::Property &property : literal->props) {
    if (property.spread()) {
      bool hasSpreadFailed = this->reportSpreadIntoRecord(property, value);
      hasFailed = hasFailed || hasSpreadFailed;
    }
  }
  literal->type =
      hasFailed ? Type::error() : Type::dict(Type::DictKind::Record, Type::string(), value);
  for (ObjectLitExpr::Property &property : llvm::reverse(literal->props)) {
    if (!property.spread()) {
      this->state.walk.tasks.pushTask(ConvertSlot{.slot = &property.value, .to = value});
    }
  }
}

SpreadLiterals::ResultDefinedSpreading SpreadLiterals::checkSpreadDefined(const Type *type) const {
  if (type->isOptional() && type->inner()->isObject() && !type->inner()->isTuple()) {
    auto message = ErrorTextSpreadLiteral::undefined();

    return err(CodeLiteralsRegistrySpreadDefinedError{
        .code = CodeLiteralsRegistrySpreadDefinedErrorCode::MAY_BE_UNDEFINED,
        .message = message,
    });
  }

  return ok(type);
}

SpreadLiterals::ResultSpreadIsAnObject SpreadLiterals::checkSpreadObject(const Type *type) const {
  bool isObject = type->isObject() && !type->isTuple() && !type->isRegExp() && !type->isMatch() &&
                  !type->isDate() && !type->isIntl() && !type->isBytes() && !type->isCrypto();
  if (!isObject) {
    bool hasIndices =
        type->nonOptional()->isArray() || type->nonOptional()->isTuple() || type->isString();
    auto message = ErrorTextSpreadLiteral::notObject(type->name(), hasIndices);

    return err(CodeLiteralsRegistrySpreadObjectError{
        .code = CodeLiteralsRegistrySpreadObjectErrorCode::NOT_AN_OBJECT,
        .message = message,
    });
  }

  return ok(type);
}

SpreadLiterals::ResultPlainSpreadClass
SpreadLiterals::checkSpreadFields(const ClassDecl *classDecl) const {
  if (classDecl->hasMethodFields()) {
    auto message = ErrorTextSpreadLiteral::methods();

    return err(CodeLiteralsRegistrySpreadFieldsError{
        .code = CodeLiteralsRegistrySpreadFieldsErrorCode::METHOD_FIELDS,
        .message = message,
    });
  }

  return ok(classDecl);
}

SpreadLiterals::ResultSpreadFieldJoins
SpreadLiterals::checkSpreadJoin(const Field &spread, const Field &field, const Type *type) const {
  const auto *base =
      this->state.types.relations.unify(field.type->nonOptional(), spread.type->inner());
  if (base == nullptr || base->isError()) {
    auto message = ErrorTextSpreadLiteral::twoTypes(spread.name, field.type->name(),
                                                    spread.type->name(), type->name());

    return err(CodeLiteralsRegistrySpreadJoinError{
        .code = CodeLiteralsRegistrySpreadJoinErrorCode::TYPES_DIFFER,
        .message = message,
    });
  }

  return ok(base);
}

bool SpreadLiterals::reportSpreadType(const ObjectLitExpr::Property &property, const Type *type) {
  auto defined = this->checkSpreadDefined(type);
  if (defined.isErr()) {
    this->state.diagnostics.error(property.value->loc(), defined.error().message);
    return true;
  }
  auto object = this->checkSpreadObject(type);
  if (object.isErr()) {
    this->state.diagnostics.error(property.value->loc(), object.error().message);
    return true;
  }

  return false;
}

bool SpreadLiterals::reportSpreadClass(const ObjectLitExpr::Property &property, const Type *type) {
  auto *classDecl = this->literalRules.classOf(type);
  if (classDecl == nullptr) {
    return false;
  }
  auto plain = this->checkSpreadFields(classDecl);
  if (plain.isErr()) {
    this->state.diagnostics.error(property.value->loc(), plain.error().message);
    return true;
  }

  return false;
}

void SpreadLiterals::mergeSpreadFields(const ObjectLitExpr::Property &property, const Type *type,
                                       SpreadDraft &collected) {
  for (const Field &spreadField : type->fields()) {
    if (!llvm::is_contained(property.omit(), spreadField.name)) {
      this->mergeSpreadField(property, spreadField, type, collected);
    }
  }
}

void SpreadLiterals::mergeSpreadField(const ObjectLitExpr::Property &property,
                                      const Field &spreadField, const Type *type,
                                      SpreadDraft &collected) {
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
    this->state.diagnostics.error(property.loc, joined.error().message);
    collected.isTyped = false;
    return;
  }
  const Type *base = joined.value();
  field->type = field->type->isOptional() ? Type::optional(base) : base;
}

Field *SpreadLiterals::findSlot(std::vector<Field> &fields, llvm::StringRef name) const {
  for (Field &field : fields) {
    if (field.name == name) {
      return &field;
    }
  }

  return nullptr;
}

void SpreadLiterals::takeOmitKeys(ObjectLitExpr::Property &property) const {
  SpreadProperty *spread = property.form.as<SpreadProperty>();
  for (const IndexExpr *read : spread->omitKeys) {
    if (!read->field().empty()) {
      spread->omit.push_back(read->field());
    }
  }
  spread->omitKeys.clear();
}

const Type *SpreadLiterals::spreadResult(const SpreadDraft &collected) const {
  const auto *result = Type::object(collected.fields);
  if (collected.target == nullptr) {
    return result;
  }
  bool isFitting = this->isEveryFieldFitting(collected.fields, collected.target) &&
                   this->isTargetFilled(collected.target, collected.fields);

  return isFitting ? collected.target : result;
}

bool SpreadLiterals::isEveryFieldFitting(const std::vector<Field> &fields,
                                         const Type *target) const {
  for (const Field &field : fields) {
    const auto *targetField = target->field(field.name);
    bool isFitting = targetField != nullptr && this->state.types.relations.isImplicitlyConvertible(
                                                   field.type, targetField->type);
    if (!isFitting) {
      return false;
    }
  }

  return true;
}

bool SpreadLiterals::isTargetFilled(const Type *target, const std::vector<Field> &fields) const {
  for (const Field &targetField : target->fields()) {
    if (!targetField.type->isOptional() &&
        this->literalRules.fieldNamed(fields, targetField.name) == nullptr) {
      return false;
    }
  }

  return true;
}

void SpreadLiterals::markReplacedFields(ObjectLitExpr *literal, size_t index) const {
  ObjectLitExpr::Property &property = literal->props[index];
  SpreadProperty *spread = property.form.as<SpreadProperty>();
  for (const Field &field : property.value->type->fields()) {
    bool isOmitted = llvm::is_contained(spread->omit, field.name);
    if (!isOmitted && this->isReplacedLater(literal, index, field.name)) {
      spread->replacedFields.push_back(field.name);
    }
  }
}

bool SpreadLiterals::isReplacedLater(const ObjectLitExpr *literal, size_t index,
                                     llvm::StringRef name) const {
  for (size_t laterIndex = index + 1; laterIndex < literal->props.size(); ++laterIndex) {
    if (this->isReplacing(literal->props[laterIndex], name)) {
      return true;
    }
  }

  return false;
}

bool SpreadLiterals::isReplacing(const ObjectLitExpr::Property &later, llvm::StringRef name) const {
  if (!later.spread()) {
    return later.name == name;
  }
  if (llvm::is_contained(later.omit(), name)) {
    return false;
  }
  const auto *field = later.value->type->field(name);

  return field != nullptr && !field->type->isOptional();
}

SpreadLiterals::ResultPlainProperty
SpreadLiterals::checkPlainProperty(const ObjectLitExpr::Property &property) const {
  if (property.defaulted()) {
    auto message = ErrorTextRecordProperty::defaulted(property.name);

    return err(CodeLiteralsRegistryPlainPropertyError{
        .code = CodeLiteralsRegistryPlainPropertyErrorCode::DEFAULT_IN_LITERAL,
        .message = message,
    });
  }
  if (property.method()) {
    auto message = ErrorTextRecordProperty::method(property.name);

    return err(CodeLiteralsRegistryPlainPropertyError{
        .code = CodeLiteralsRegistryPlainPropertyErrorCode::METHOD_IN_RECORD,
        .message = message,
    });
  }

  return ok(&property);
}

void SpreadLiterals::reportSpread(const ObjectLitExpr::Property &property,
                                  const CodeLiteralsRegistryCheckRecordSpreadError &error) {
  auto at = this->spreadErrorPlace(property, error.code);
  this->state.diagnostics.error(at, error.message);
}

llvm::SMLoc
SpreadLiterals::spreadErrorPlace(const ObjectLitExpr::Property &property,
                                 CodeLiteralsRegistryCheckRecordSpreadErrorCode code) const {
  switch (code) {
    case CodeLiteralsRegistryCheckRecordSpreadErrorCode::KEYED_REST:
      return property.loc;
    case CodeLiteralsRegistryCheckRecordSpreadErrorCode::MAY_BE_UNDEFINED:
    case CodeLiteralsRegistryCheckRecordSpreadErrorCode::OTHER_VALUES:
    case CodeLiteralsRegistryCheckRecordSpreadErrorCode::FIELD_TYPE_DIFFERS:
    case CodeLiteralsRegistryCheckRecordSpreadErrorCode::HAS_METHODS:
    case CodeLiteralsRegistryCheckRecordSpreadErrorCode::NOT_SPREADABLE:
      return property.value->loc();
  }

  return property.value->loc();
}

bool SpreadLiterals::reportSpreadIntoRecord(ObjectLitExpr::Property &property, const Type *value) {
  auto spread = this->checkRecordSpread(property, value);
  if (spread.isOk()) {
    return false;
  }
  this->reportSpread(property, spread.error());

  return true;
}

SpreadLiterals::ResultSpreadIntoMap
SpreadLiterals::checkRecordSpread(ObjectLitExpr::Property &property, const Type *value) {
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

SpreadLiterals::ResultSpreadIntoMap
SpreadLiterals::checkSpreadSource(const ObjectLitExpr::Property &property) const {
  const Type *type = property.value->type;
  if (type->isOptional() && (type->inner()->isRecord() || type->inner()->isObject())) {
    auto message = ErrorTextRecordSpread::undefined();

    return err(CodeLiteralsRegistryCheckRecordSpreadError{
        .code = CodeLiteralsRegistryCheckRecordSpreadErrorCode::MAY_BE_UNDEFINED,
        .message = message,
    });
  }
  if (!property.omitKeys().empty()) {
    auto message = ErrorTextRecordSpread::keyedRest();

    return err(CodeLiteralsRegistryCheckRecordSpreadError{
        .code = CodeLiteralsRegistryCheckRecordSpreadErrorCode::KEYED_REST,
        .message = message,
    });
  }

  return ok(type);
}

SpreadLiterals::ResultSpreadIntoMap SpreadLiterals::checkRecordValues(const Type *type,
                                                                      const Type *value) const {
  if (!this->state.types.relations.isImplicitlyConvertible(type->valueType(), value)) {
    auto message = ErrorTextRecordSpread::values(type->name(), value->name());

    return err(CodeLiteralsRegistryCheckRecordSpreadError{
        .code = CodeLiteralsRegistryCheckRecordSpreadErrorCode::OTHER_VALUES,
        .message = message,
    });
  }

  return ok(type);
}

SpreadLiterals::ResultSpreadIntoMap
SpreadLiterals::checkObjectSpread(const ObjectLitExpr::Property &property, const Type *value) {
  const Type *type = property.value->type;
  bool isObject = type->isObject() && !type->isTuple() && !type->isRegExp() && !type->isMatch() &&
                  !type->isAnyObject();
  if (!isObject) {
    bool isCollection = type->isMap() || type->isSet();
    auto message = ErrorTextRecordSpread::notObject(type->name(), isCollection);

    return err(CodeLiteralsRegistryCheckRecordSpreadError{
        .code = CodeLiteralsRegistryCheckRecordSpreadErrorCode::NOT_SPREADABLE,
        .message = message,
    });
  }
  auto fields = this->checkRecordSpreadFields(property, value);
  if (fields.isErr()) {
    return err(fields.error());
  }
  if (this->hasClassMethods(type)) {
    auto message = ErrorTextRecordSpread::methods();

    return err(CodeLiteralsRegistryCheckRecordSpreadError{
        .code = CodeLiteralsRegistryCheckRecordSpreadErrorCode::HAS_METHODS,
        .message = message,
    });
  }

  return ok(type);
}

SpreadLiterals::ResultSpreadIntoMap
SpreadLiterals::checkRecordSpreadFields(const ObjectLitExpr::Property &property,
                                        const Type *value) const {
  const Type *type = property.value->type;
  for (const Field &field : type->fields()) {
    bool isKept = !llvm::is_contained(property.omit(), field.name);
    const Type *candidate =
        field.type->isOptional() && !value->isOptional() ? field.type->inner() : field.type;
    if (isKept && !this->state.types.relations.isImplicitlyConvertible(candidate, value)) {
      auto message =
          ErrorTextRecordSpread::field(field.name, type->name(), field.type->name(), value->name());

      return err(CodeLiteralsRegistryCheckRecordSpreadError{
          .code = CodeLiteralsRegistryCheckRecordSpreadErrorCode::FIELD_TYPE_DIFFERS,
          .message = message,
      });
    }
  }

  return ok(type);
}

bool SpreadLiterals::hasClassMethods(const Type *type) const {
  const ClassDecl *classDecl = this->literalRules.classOf(type);
  for (; classDecl != nullptr; classDecl = classDecl->parent) {
    bool hasMethodField =
        llvm::any_of(classDecl->fields, std::bind_front(&SpreadLiterals::isMethodField, this));
    if (classDecl->literal() != nullptr || hasMethodField) {
      return true;
    }
  }

  return false;
}

bool SpreadLiterals::isMethodField(const ClassField &field) const {
  return field.method() != nullptr;
}

SpreadLiterals::ResultValueTypeJoin SpreadLiterals::checkValuesJoin(const Type *value,
                                                                    const Type *type) const {
  const Type *next = value != nullptr ? this->state.types.relations.unify(value, type) : type;
  if (next == nullptr || next->isError()) {
    auto message = ErrorTextRecordValues::differ(value->name(), type->name());

    return err(CodeLiteralsRegistryValuesJoinError{
        .code = CodeLiteralsRegistryValuesJoinErrorCode::VALUE_TYPES_DIFFER,
        .message = message,
    });
  }

  return ok(next);
}

SpreadLiterals::ResultValueTypeKnow SpreadLiterals::checkValueKnown(const Type *value) const {
  if (value->isUndefined() || value->isVoid()) {
    auto message = ErrorTextRecordValue::undefined();

    return err(CodeLiteralsRegistryValueKnownError{
        .code = CodeLiteralsRegistryValueKnownErrorCode::ONLY_UNDEFINED,
        .message = message,
    });
  }

  return ok(value);
}

std::vector<SpreadLiterals::ValueCandidate>
SpreadLiterals::valueCandidates(const ObjectLitExpr *literal) const {
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

void SpreadLiterals::addFieldCandidates(const ObjectLitExpr::Property &property,
                                        std::vector<ValueCandidate> &candidates) const {
  for (const Field &field : property.value->type->fields()) {
    if (!llvm::is_contained(property.omit(), field.name)) {
      const Type *type = field.type->isOptional() ? field.type->inner() : field.type;
      candidates.push_back(ValueCandidate{.type = type, .loc = property.value->loc()});
    }
  }
}

SpreadLiterals::RecordValue SpreadLiterals::joinValue(const RecordValue &current,
                                                      const ValueCandidate &candidate) {
  if (candidate.type == nullptr || candidate.type->isError()) {
    return RecordValue{
        .value = current.value,
        .isTyped = false,
    };
  }
  auto joined = this->checkValuesJoin(current.value, candidate.type);
  if (joined.isErr()) {
    this->state.diagnostics.error(candidate.loc, joined.error().message);
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
