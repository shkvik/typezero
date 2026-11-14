#include "compiler/semantic/code/dictionaries/dictionary_sources/dictionary_sources.h"
#include "llvm/ADT/STLExtras.h"

using llvm::cast;

namespace tz {

DictionarySources::DictionarySources(SemanticState &owner, DictionaryRules &sharedDictionaryRules)
    : state(owner),
      dictionaryRules(sharedDictionaryRules) {}

void DictionarySources::nextPairPart(const NextPairPart &task) {
  const PairsDraft &draft = task.draft;
  if (draft.element == draft.literal->elements.size()) {
    return this->finishInferredPairs(draft);
  }
  auto *pair = cast<ArrayLitExpr>(draft.literal->elements[draft.element].expr);
  Expr **part = &pair->elements[draft.isValue ? 1 : 0].expr;
  const Type *common = draft.isValue ? draft.value : draft.key;
  bool isLiteralPart = this->state.code.conversions.isNumberLiteralTree(*part);
  const Type *hint = common != nullptr && isLiteralPart ? common : nullptr;
  this->state.walk.tasks.pushTask(AfterPairPart{.draft = draft});

  return this->state.walk.tasks.pushTask(CheckExprSlot{.slot = part, .expected = hint});
}

void DictionarySources::afterPairPart(const AfterPairPart &task) {
  auto draft = task.draft;
  auto *pair = cast<ArrayLitExpr>(draft.literal->elements[draft.element].expr);
  const Expr *part = pair->elements[draft.isValue ? 1 : 0].expr;
  if (part->type->isError()) {
    draft.isTyped = false;
  } else {
    this->joinPairPart(draft, part);
  }
  draft.element = draft.isValue ? draft.element + 1 : draft.element;
  draft.isValue = !draft.isValue;

  return this->nextPairPart(NextPairPart{.draft = draft});
}

void DictionarySources::joinPairPart(PairsDraft &draft, const Expr *part) {
  const Type *common = draft.isValue ? draft.value : draft.key;
  auto which = draft.isValue ? PairPart::VALUE : PairPart::KEY;
  auto joined = this->checkPartsJoin(which, common, part->type);
  if (joined.isErr()) {
    this->state.diagnostics.error(part->loc(), joined.error().message);
    draft.isTyped = false;
    return;
  }
  if (draft.isValue) {
    draft.value = joined.value();
    return;
  }
  draft.key = joined.value();
}

void DictionarySources::finishInferredPairs(const PairsDraft &draft) {
  NewExpr *expr = draft.newExpr;
  ArrayLitExpr *literal = draft.literal;
  if (!draft.isTyped || draft.key == nullptr || draft.value == nullptr) {
    expr->type = Type::error();
    return this->state.walk.tasks.pushChecked(expr);
  }
  auto known = this->checkPairsKnown(draft.key, draft.value);
  if (known.isErr()) {
    this->state.diagnostics.error(literal->loc(), known.error().message);
    expr->type = Type::error();
    return this->state.walk.tasks.pushChecked(expr);
  }
  const auto *pairType = Type::tuple({draft.key, draft.value});
  for (const ArrayLitExpr::Element &element : literal->elements) {
    element.expr->type = pairType;
  }
  literal->type = Type::array(pairType);
  this->state.walk.tasks.pushTask(
      FinishPairsDict{.newExpr = expr, .key = draft.key, .value = draft.value});

  return this->pushPairCoercions(literal, draft.key, draft.value);
}

void DictionarySources::pushPairCoercions(ArrayLitExpr *literal, const Type *key,
                                          const Type *value) {
  for (const ArrayLitExpr::Element &element : llvm::reverse(literal->elements)) {
    auto *pair = cast<ArrayLitExpr>(element.expr);
    this->state.walk.tasks.pushTask(
        CoerceSlot{.slot = &pair->elements[1].expr, .to = value, .what = ""});
    this->state.walk.tasks.pushTask(
        CoerceSlot{.slot = &pair->elements[0].expr, .to = key, .what = ""});
  }
}

void DictionarySources::finishPairsDict(const FinishPairsDict &task) {
  NewExpr *expr = task.newExpr;
  Expr *arg = expr->args[0];
  auto allowed = this->dictionaryRules.checkKeyType(task.key);
  if (allowed.isErr()) {
    this->state.diagnostics.error(arg->loc(), allowed.error().message);
    expr->type = Type::error();
    return this->state.walk.tasks.pushChecked(expr);
  }
  const auto *type = Type::dict(Type::DictKind::Map, task.key, task.value);

  return this->dictionaryRules.finishDictNew(expr, type, {arg});
}

void DictionarySources::afterDictSource(const AfterDictSource &task) {
  NewExpr *expr = task.newExpr;
  const Expr *iterable = expr->args[0];
  const Type *type = iterable->type;
  if (type->isError()) {
    expr->type = Type::error();
    return this->state.walk.tasks.pushChecked(expr);
  }
  this->state.walk.tasks.pushTask(
      FinishDictSource{.newExpr = expr, .type = task.type, .isMap = task.isMap});
  const auto *given = this->state.code.iteration.iteratedType(type);
  if (given == nullptr || !type->isIterator()) {
    return;
  }
  auto what = ErrorTextDictSource::newName(expr->name);
  auto nothing = this->state.code.iteration.checkSendsNothing(type, what);
  if (nothing.isErr()) {
    this->state.diagnostics.error(iterable->loc(), nothing.error().message);
  }

  return this->state.walk.tasks.pushTask(CalledFunction{});
}

void DictionarySources::finishDictSource(const FinishDictSource &task) {
  NewExpr *expr = task.newExpr;
  Expr *iterable = expr->args[0];
  const Type *type = iterable->type;
  const auto *wanted = this->dictionaryRules.wantedElement(task.type, task.isMap);
  auto what = ErrorTextDictSource::newName(expr->name);
  auto items = task.isMap ? SourceItems::PAIRS : SourceItems::VALUES;
  const auto *given = this->state.code.iteration.iteratedType(type);
  auto shaped = this->checkSourceShape(type, given, wanted, items, what);
  if (shaped.isErr()) {
    this->state.diagnostics.error(iterable->loc(), shaped.error().message);
    expr->type = Type::error();
    return this->state.walk.tasks.pushChecked(expr);
  }
  if (task.type != nullptr) {
    return this->dictionaryRules.finishDictNew(expr, task.type, {iterable});
  }
  auto made = this->sourceElementDict(expr, iterable, given, task.isMap);
  if (!made.has_value()) {
    expr->type = Type::error();
    return this->state.walk.tasks.pushChecked(expr);
  }

  return this->dictionaryRules.finishDictNew(expr, *made, {iterable});
}

DictionarySources::ResultPartsJoined
DictionarySources::checkPartsJoin(PairPart part, const Type *common, const Type *type) const {
  const Type *next = common != nullptr ? this->state.types.relations.unify(common, type) : type;
  if (next == nullptr && part == PairPart::KEY) {
    auto message = ErrorTextPairParts::keysDiffer(common->name(), type->name());

    return err(CodeDictionariesRegistryPartsJoinError{
        .code = CodeDictionariesRegistryPartsJoinErrorCode::KEYS_DIFFER,
        .message = message,
    });
  }
  if (next == nullptr) {
    auto message = ErrorTextPairParts::valuesDiffer(common->name(), type->name());

    return err(CodeDictionariesRegistryPartsJoinError{
        .code = CodeDictionariesRegistryPartsJoinErrorCode::VALUES_DIFFER,
        .message = message,
    });
  }

  return ok(next);
}

DictionarySources::ResultPairTypesOk DictionarySources::checkPairsKnown(const Type *key,
                                                                        const Type *value) const {
  if (key->isUndefined() || value->isUndefined() || key->isVoid() || value->isVoid()) {
    auto message = ErrorTextPairsKnown::undefined();

    return err(CodeDictionariesRegistryPairsKnownError{
        .code = CodeDictionariesRegistryPairsKnownErrorCode::TYPES_UNDEFINED,
        .message = message,
    });
  }

  return ok(key);
}

DictionarySources::ResultSourceShape
DictionarySources::checkSourceShape(const Type *type, const Type *given, const Type *wanted,
                                    SourceItems items, llvm::StringRef what) const {
  if (given == nullptr) {
    auto message = ErrorTextDictSource::notIter(what, type->name(), type->isRecord());

    return err(CodeDictionariesRegistrySourceShapeError{
        .code = CodeDictionariesRegistrySourceShapeErrorCode::NOT_ITERABLE,
        .message = message,
    });
  }
  if (items == SourceItems::PAIRS && !this->isFittingPair(given, wanted)) {
    llvm::StringRef wantedName = wanted != nullptr ? wanted->name() : llvm::StringRef();
    auto message = ErrorTextDictSource::notPairs(what, wantedName, given->name(), given->isArray());

    return err(CodeDictionariesRegistrySourceShapeError{
        .code = CodeDictionariesRegistrySourceShapeErrorCode::NOT_PAIRS,
        .message = message,
    });
  }
  bool isWrongValue = items == SourceItems::VALUES && wanted != nullptr &&
                      !this->state.types.relations.isImplicitlyConvertible(given, wanted);
  if (isWrongValue) {
    auto message = ErrorTextDictSource::values(what, wanted->name(), given->name());

    return err(CodeDictionariesRegistrySourceShapeError{
        .code = CodeDictionariesRegistrySourceShapeErrorCode::WRONG_VALUES,
        .message = message,
    });
  }

  return ok(given);
}

bool DictionarySources::isFittingPair(const Type *given, const Type *wanted) const {
  bool isPair = given->isTuple() && given->fields().size() == 2;
  if (!isPair || wanted == nullptr) {
    return isPair;
  }
  bool isKeyFitting = this->state.types.relations.isImplicitlyConvertible(given->fields()[0].type,
                                                                          wanted->fields()[0].type);
  bool isValueFitting = this->state.types.relations.isImplicitlyConvertible(
      given->fields()[1].type, wanted->fields()[1].type);

  return isKeyFitting && isValueFitting;
}

DictionarySources::ResultPairsShaped DictionarySources::checkPairShape(const Type *element) const {
  if (!element->isTuple() || element->fields().size() != 2) {
    auto message = ErrorTextDictPairs::pairs(element->name());

    return err(CodeDictionariesRegistryPairShapeError{
        .code = CodeDictionariesRegistryPairShapeErrorCode::NOT_PAIRS,
        .message = message,
    });
  }

  return ok(element);
}

DictionarySources::ResultDefinedType DictionarySources::checkElementsKnown(const NewExpr *expr,
                                                                           const Type *key,
                                                                           const Type *element,
                                                                           bool isMap) const {
  if (key->isUndefined() || (isMap && element->fields()[1].type->isUndefined())) {
    auto message = ErrorTextDictElements::undefined(expr->name);

    return err(CodeDictionariesRegistryElementsKnownError{
        .code = CodeDictionariesRegistryElementsKnownErrorCode::TYPES_UNDEFINED,
        .message = message,
    });
  }

  return ok(element);
}

std::optional<const Type *> DictionarySources::sourceElementDict(const NewExpr *expr,
                                                                 const Expr *arg,
                                                                 const Type *element, bool isMap) {
  auto shaped = this->checkPairShape(element);
  if (isMap && shaped.isErr()) {
    this->state.diagnostics.error(arg->loc(), shaped.error().message);
    return std::nullopt;
  }
  const Type *key = isMap ? element->fields()[0].type : element;
  auto allowed = this->dictionaryRules.checkKeyType(key);
  if (allowed.isErr()) {
    this->state.diagnostics.error(arg->loc(), allowed.error().message);
    return std::nullopt;
  }
  auto defined = this->checkElementsKnown(expr, key, element, isMap);
  if (defined.isErr()) {
    this->state.diagnostics.error(arg->loc(), defined.error().message);
    return std::nullopt;
  }
  if (isMap) {
    return Type::dict(Type::DictKind::Map, key, element->fields()[1].type);
  }

  return Type::dict(Type::DictKind::Set, key, Type::voidTy());
}

}
