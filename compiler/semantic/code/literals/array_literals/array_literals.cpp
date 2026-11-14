#include "compiler/semantic/code/literals/array_literals/array_literals.h"
#include "compiler/semantic/generics/generic_check_helpers/generic_check_helpers.h"
#include "llvm/ADT/STLExtras.h"

namespace tz {

namespace {

constexpr const char *kTupleSeparator = ", ";

}

ArrayLiterals::ArrayLiterals(SemanticState &owner)
    : state(owner) {}

void ArrayLiterals::checkArrayLiteral(const CheckArrayLiteral &task) {
  ArrayLitExpr *literal = task.literal;
  const Type *expected = task.expected;
  auto &tasks = this->state.walk.tasks;
  bool isPairLiteral = this->state.code.literals.isPairLiteral(literal);
  this->state.code.literals.forgetPairLiteral(literal);
  if (isPairLiteral && expected == nullptr) {
    tasks.pushTask(FinishPairLiteral{.literal = literal});
    return this->pushLooseElements(literal);
  }
  if (expected != nullptr && expected->nonOptional()->isJson()) {
    return tasks.pushTask(CheckJsonArrayLit{.literal = literal});
  }
  const auto *wanted = this->wantedArray(expected);
  const auto *expectedElement = this->wantedElement(wanted);
  for (const ArrayLitExpr::Element &hole : literal->elements) {
    auto given = this->checkElementGiven(hole);
    if (given.isErr()) {
      this->state.diagnostics.error(hole.expr->loc(), given.error().message);
      literal->type = Type::error();
      tasks.pushChecked(literal);
      return this->pushLooseElements(literal);
    }
  }
  if (wanted != nullptr && wanted->nonOptional()->isTuple()) {
    return this->startTupleLit(literal, wanted->nonOptional());
  }
  auto draft = ArrayDraft{
      .literal = literal,
      .expectedElement = expectedElement,
      .types = std::vector<const Type *>(literal->elements.size()),
      .hasDecimal = false,
      .seen = nullptr,
      .literalHint = nullptr,
      .isUnseen = false,
      .index = 0,
  };

  return this->nextArrayElement(NextArrayElement{.draft = std::move(draft)});
}

void ArrayLiterals::pushLooseElements(ArrayLitExpr *literal) {
  for (ArrayLitExpr::Element &element : llvm::reverse(literal->elements)) {
    this->state.walk.tasks.pushTask(CheckExprSlot{.slot = &element.expr, .expected = nullptr});
  }
}

void ArrayLiterals::finishPairLiteral(const FinishPairLiteral &task) {
  ArrayLitExpr *literal = task.literal;
  auto parts = std::vector<const Type *>();
  bool isBad = false;
  for (const ArrayLitExpr::Element &element : literal->elements) {
    const Type *part = element.expr->type;
    parts.push_back(part);
    isBad = isBad || this->isBadPart(part);
  }
  literal->type = isBad ? Type::error() : Type::tuple(parts);

  return this->state.walk.tasks.pushChecked(literal);
}

void ArrayLiterals::nextArrayElement(const NextArrayElement &task) {
  auto draft = task.draft;
  ArrayLitExpr *literal = draft.literal;
  auto &tasks = this->state.walk.tasks;
  while (draft.index < literal->elements.size()) {
    ArrayLitExpr::Element &element = literal->elements[draft.index];
    bool isSkipped = draft.isUnseen
                         ? draft.types[draft.index] != nullptr
                         : !element.spread && draft.expectedElement == nullptr &&
                               this->state.code.conversions.isNumberLiteralTree(element.expr);
    if (!isSkipped) {
      break;
    }
    draft.index += 1;
  }
  if (draft.index == literal->elements.size()) {
    return this->afterArrayPass(std::move(draft));
  }
  ArrayLitExpr::Element &element = literal->elements[draft.index];
  if (draft.isUnseen) {
    const Type *hint = draft.literalHint;
    tasks.pushTask(AfterLiteralElement{.draft = std::move(draft)});
    return tasks.pushTask(CheckExprSlot{.slot = &element.expr, .expected = hint});
  }
  if (!element.spread) {
    bool isShaped = draft.expectedElement == nullptr && draft.seen != nullptr &&
                    draft.seen->holdsObjects() &&
                    this->state.inference.destinations.isLiteralShaped(element.expr);
    const Type *hint = isShaped ? draft.seen : draft.expectedElement;
    tasks.pushTask(AfterLiteralElement{.draft = std::move(draft)});
    return tasks.pushTask(CheckExprSlot{.slot = &element.expr, .expected = hint});
  }
  const Type *hint =
      draft.expectedElement != nullptr ? Type::array(draft.expectedElement) : nullptr;
  tasks.pushTask(AfterSpreadElement{.draft = std::move(draft)});
  tasks.pushTask(IterableOfSlot{.slot = &element.expr});
  tasks.pushTask(CheckExprSlot{.slot = &element.expr, .expected = hint});

  return tasks.pushTask(MarkFused{.expr = element.expr});
}

void ArrayLiterals::afterLiteralElement(const AfterLiteralElement &task) {
  auto draft = task.draft;
  const Type *type = draft.literal->elements[draft.index].expr->type;
  draft.types[draft.index] = type;
  draft.index += 1;
  if (draft.isUnseen) {
    return this->nextArrayElement(NextArrayElement{.draft = std::move(draft)});
  }
  draft.hasDecimal = draft.hasDecimal || type->nonOptional()->isDecimal();
  if (!type->isError() && draft.seen != Type::error()) {
    draft.seen = this->seenJoined(draft.seen, type);
  }
  if (draft.seen == nullptr) {
    draft.seen = Type::error();
  }

  return this->nextArrayElement(NextArrayElement{.draft = std::move(draft)});
}

void ArrayLiterals::afterSpreadElement(const AfterSpreadElement &task) {
  auto draft = task.draft;
  ArrayLitExpr::Element &element = draft.literal->elements[draft.index];
  if (!element.expr->type->isMatch()) {
    return this->finishSpreadElement(std::move(draft));
  }
  this->state.walk.tasks.pushTask(AfterSpreadMatch{.draft = std::move(draft)});
  this->state.walk.tasks.pushChecked(element.expr);

  return this->state.walk.tasks.pushTask(MatchItems{});
}

void ArrayLiterals::afterSpreadMatch(const AfterSpreadMatch &task) {
  auto draft = task.draft;
  draft.literal->elements[draft.index].expr = this->state.walk.tasks.takeChecked();

  return this->finishSpreadElement(std::move(draft));
}

void ArrayLiterals::finishSpreadElement(ArrayDraft draft) {
  Expr *spread = draft.literal->elements[draft.index].expr;
  const Type *type = spread->type;
  size_t index = draft.index;
  draft.index += 1;
  if (type->isString()) {
    draft.types[index] = type;
  } else if (type->isIterator()) {
    auto what = ErrorTextSpreadable::spread();
    auto nothing = this->state.code.iteration.checkSendsNothing(type, what);
    if (nothing.isErr()) {
      this->state.diagnostics.error(spread->loc(), nothing.error().message);
    }
    this->addSpreadType(draft, index, type->yielded());
    this->state.walk.tasks.pushTask(NextArrayElement{.draft = std::move(draft)});
    return this->state.walk.tasks.pushTask(CalledFunction{});
  } else if (type->isTuple()) {
    draft.types[index] = this->tupleSpreadType(spread);
  } else if (type->isTypedArray()) {
    draft.types[index] = type->elementType();
  } else {
    const auto *spreadElement = this->arraySpreadType(spread);
    this->addSpreadType(draft, index, spreadElement);
  }

  return this->nextArrayElement(NextArrayElement{.draft = std::move(draft)});
}

void ArrayLiterals::addSpreadType(ArrayDraft &draft, size_t index, const Type *type) {
  draft.types[index] = type;
  draft.hasDecimal = draft.hasDecimal || type->nonOptional()->isDecimal();
}

void ArrayLiterals::afterArrayPass(ArrayDraft draft) {
  if (!draft.isUnseen) {
    draft.literalHint = this->literalHint(draft.literal, draft);
    draft.isUnseen = true;
    draft.index = 0;
    return this->nextArrayElement(NextArrayElement{.draft = std::move(draft)});
  }
  ArrayLitExpr *literal = draft.literal;
  literal->type = Type::error();
  const Type *element = draft.expectedElement != nullptr
                            ? draft.expectedElement
                            : this->joinedElement(literal, draft.types);
  this->state.walk.tasks.pushChecked(literal);
  if (element == nullptr || element->isError()) {
    return;
  }
  const auto *arrayType = Type::array(element);
  literal->type = arrayType;
  for (ArrayLitExpr::Element &item : llvm::reverse(literal->elements)) {
    if (!item.spread) {
      this->state.walk.tasks.pushTask(CoerceSlot{.slot = &item.expr, .to = element, .what = ""});
      continue;
    }
    auto goes = this->checkSpreadFits(item.expr->type, element, arrayType);
    if (goes.isErr()) {
      this->state.diagnostics.error(item.expr->loc(), goes.error().message);
    }
  }
}

void ArrayLiterals::startTupleLit(ArrayLitExpr *literal, const Type *tuple) {
  llvm::ArrayRef<Field> fields = tuple->fields();
  literal->type = Type::error();
  auto &tasks = this->state.walk.tasks;
  const Expr *spread = this->firstSpread(literal);
  bool isSpread = spread != literal;
  if (isSpread || literal->elements.size() != fields.size()) {
    auto message = isSpread ? ErrorTextTupleLit::spread(tuple->name())
                            : ErrorTextTupleLit::length(tuple->name(), fields.size(),
                                                        literal->elements.size());
    llvm::SMLoc at = isSpread ? spread->loc() : literal->loc();
    tasks.pushChecked(literal);
    tasks.pushTask(ReportError{.loc = at, .message = message});
    return this->pushLooseElements(literal);
  }
  literal->type = tuple;
  tasks.pushChecked(literal);
  for (size_t index = fields.size(); index > 0; --index) {
    Expr **slot = &literal->elements[index - 1].expr;
    const Type *type = fields[index - 1].type;
    auto what = ErrorTextTupleLit::element(index - 1);
    tasks.pushTask(CoerceSlot{.slot = slot, .to = type, .what = what});
    tasks.pushTask(CheckExprSlot{.slot = slot, .expected = type});
  }
}

ArrayLiterals::ResultElementGiven
ArrayLiterals::checkElementGiven(const ArrayLitExpr::Element &element) const {
  if (element.hole) {
    auto message = ErrorTextArrayHole::hole();

    return err(CodeLiteralsRegistryElementGivenError{
        .code = CodeLiteralsRegistryElementGivenErrorCode::HOLE_IN_ARRAY,
        .message = message,
    });
  }

  return ok(element.expr);
}

ArrayLiterals::ResultTupleElement ArrayLiterals::checkTupleSpread(const Type *type) const {
  const auto *element = this->tupleElement(type);
  if (element == nullptr) {
    auto message = ErrorTextTupleSpread::differ(type->name());

    return err(CodeLiteralsRegistryTupleSpreadError{
        .code = CodeLiteralsRegistryTupleSpreadErrorCode::ELEMENTS_DIFFER,
        .message = message,
    });
  }

  return ok(element);
}

ArrayLiterals::ResultSpreadSource ArrayLiterals::checkSpreadable(const Type *type) const {
  if (!type->isArray() && !type->isError()) {
    auto message = ErrorTextSpreadable::notSpread(type->name(), type->isRecord());

    return err(CodeLiteralsRegistrySpreadableError{
        .code = CodeLiteralsRegistrySpreadableErrorCode::NOT_SPREADABLE,
        .message = message,
    });
  }

  return ok(type);
}

ArrayLiterals::ResultArrayHasItem ArrayLiterals::checkNotEmpty(const ArrayLitExpr *literal) const {
  if (literal->elements.empty()) {
    auto message = ErrorTextArrayEmpty::empty();

    return err(CodeLiteralsRegistryNotEmptyError{
        .code = CodeLiteralsRegistryNotEmptyErrorCode::EMPTY_ARRAY,
        .message = message,
    });
  }

  return ok(literal);
}

ArrayLiterals::ResultElementsJoin ArrayLiterals::checkTypesJoin(const ArrayLitExpr *literal,
                                                                llvm::ArrayRef<const Type *> types,
                                                                const Type *element,
                                                                const Type *type) const {
  const Type *next =
      element != nullptr ? this->state.types.literals.unionUnify(element, type) : type;
  if (next != nullptr) {
    return ok(next);
  }

  auto message = ErrorTextArrayTypes::differ(element->name(), type->name());
  message += this->joinHint(literal, types, element, type);

  return err(CodeLiteralsRegistryTypesJoinError{
      .code = CodeLiteralsRegistryTypesJoinErrorCode::ELEMENT_TYPES_DIFFER,
      .message = message,
  });
}

std::string ArrayLiterals::joinHint(const ArrayLitExpr *literal, llvm::ArrayRef<const Type *> types,
                                    const Type *element, const Type *type) const {
  if (this->isLiteralClass(element) && this->isLiteralClass(type)) {
    return ErrorTextArrayTypes::literals();
  }
  if (this->hasSpread(literal) || this->hasFailedType(types)) {
    return "";
  }
  auto tuple = GenericCheckHelpers::typeNames(types, kTupleSeparator);

  return ErrorTextArrayTypes::tuple(tuple);
}

bool ArrayLiterals::isLiteralClass(const Type *type) const {
  const ClassDecl *classDecl = type->nonOptional()->classDecl();

  return classDecl != nullptr && classDecl->literal() != nullptr;
}

bool ArrayLiterals::hasSpread(const ArrayLitExpr *literal) const {
  for (const ArrayLitExpr::Element &element : literal->elements) {
    if (element.spread) {
      return true;
    }
  }

  return false;
}

bool ArrayLiterals::hasFailedType(llvm::ArrayRef<const Type *> types) const {
  for (const Type *type : types) {
    if (type->isError()) {
      return true;
    }
  }

  return false;
}

ArrayLiterals::ResultElementKnown ArrayLiterals::checkElementKnown(const Type *element) const {
  if (element->isUndefined() || element->isVoid()) {
    auto message = ErrorTextArrayElement::alone(element->name());

    return err(CodeLiteralsRegistryElementKnownError{
        .code = CodeLiteralsRegistryElementKnownErrorCode::ONLY_UNDEFINED,
        .message = message,
    });
  }

  return ok(element);
}

ArrayLiterals::ResultSpreadFitsIn
ArrayLiterals::checkSpreadFits(const Type *type, const Type *element, const Type *arrayType) const {
  if (!this->isSpreadFitting(type, element, arrayType)) {
    auto message = ErrorTextSpreadFits::into(type->name(), arrayType->name());

    return err(CodeLiteralsRegistrySpreadFitsError{
        .code = CodeLiteralsRegistrySpreadFitsErrorCode::SPREAD_MISMATCH,
        .message = message,
    });
  }

  return ok(type);
}

bool ArrayLiterals::isSpreadFitting(const Type *type, const Type *element,
                                    const Type *arrayType) const {
  if (type == arrayType || type->isError()) {
    return true;
  }
  if (type->isString() && element->isString()) {
    return true;
  }
  if (type->isArray() && this->isFittingObjectArray(type, element)) {
    return true;
  }
  if (type->isIterator() &&
      this->state.types.relations.isImplicitlyConvertible(type->yielded(), element)) {
    return true;
  }
  if (type->isTypedArray() &&
      this->state.types.relations.isImplicitlyConvertible(type->elementType(), element)) {
    return true;
  }

  return type->isTuple() && this->areFieldsConvertible(type, element);
}

bool ArrayLiterals::isFittingObjectArray(const Type *type, const Type *element) const {
  const Type *inner = type->inner();
  bool isObjects =
      UnknownChecker::isObjectReference(inner->nonOptional()) && element->nonOptional()->isObject();
  bool isOptionalFitting = inner->isOptional() == element->isOptional() || element->isOptional();

  return isObjects && isOptionalFitting &&
         this->state.types.relations.isImplicitlyConvertible(inner, element);
}

bool ArrayLiterals::areFieldsConvertible(const Type *tuple, const Type *element) const {
  for (const Field &field : tuple->fields()) {
    if (!this->state.types.relations.isImplicitlyConvertible(field.type, element)) {
      return false;
    }
  }

  return true;
}

const Type *ArrayLiterals::wantedElement(const Type *wanted) const {
  if (wanted == nullptr || !wanted->nonOptional()->isArray()) {
    return nullptr;
  }

  return wanted->nonOptional()->inner();
}

bool ArrayLiterals::isBadPart(const Type *part) const {
  return part->isError() || part->isVoid() || part->isUndefined();
}

const Type *ArrayLiterals::wantedArray(const Type *expected) const {
  if (expected == nullptr) {
    return nullptr;
  }
  if (expected->isUnknown()) {
    return Type::array(Type::unknown());
  }
  if (!expected->nonOptional()->isUnion()) {
    return expected;
  }
  const auto *array = expected->nonOptional()->slotPart(UnionSlot::Array);
  if (array != nullptr && array->isArray()) {
    return array;
  }

  return expected;
}

const Expr *ArrayLiterals::firstSpread(const ArrayLitExpr *literal) const {
  for (const ArrayLitExpr::Element &element : literal->elements) {
    if (element.spread) {
      return element.expr;
    }
  }

  return literal;
}

const Type *ArrayLiterals::seenJoined(const Type *seen, const Type *type) const {
  if (seen == nullptr) {
    return type;
  }

  return this->state.types.relations.unify(seen, type);
}

const Type *ArrayLiterals::tupleSpreadType(const Expr *spread) {
  auto element = this->checkTupleSpread(spread->type);
  if (element.isErr()) {
    this->state.diagnostics.error(spread->loc(), element.error().message);
    return Type::error();
  }

  return element.value();
}

const Type *ArrayLiterals::arraySpreadType(const Expr *spread) {
  const Type *type = spread->type;
  auto spreadableTo = this->checkSpreadable(type);
  if (spreadableTo.isErr()) {
    this->state.diagnostics.error(spread->loc(), spreadableTo.error().message);
    return Type::error();
  }

  return type->isError() ? type : type->inner();
}

const Type *ArrayLiterals::literalHint(const ArrayLitExpr *literal,
                                       const ArrayDraft &elements) const {
  if (elements.hasDecimal) {
    return Type::decimalTy();
  }
  if (elements.seen != nullptr && elements.seen->isNumeric()) {
    return elements.seen;
  }
  for (size_t index = 0; index < literal->elements.size(); ++index) {
    const Type *type = elements.types[index];
    if (literal->elements[index].spread && type != nullptr && type->isNumeric()) {
      return type;
    }
  }
  for (size_t index = 0; index < literal->elements.size(); ++index) {
    bool isFractional =
        elements.types[index] == nullptr &&
        !this->state.code.conversions.isIntLiteralTree(literal->elements[index].expr);
    if (isFractional) {
      return Type::doubleTy();
    }
  }

  return nullptr;
}

const Type *ArrayLiterals::joinedElement(const ArrayLitExpr *literal,
                                         llvm::ArrayRef<const Type *> types) {
  auto filled = this->checkNotEmpty(literal);
  if (filled.isErr()) {
    this->state.diagnostics.error(literal->loc(), filled.error().message);
    return nullptr;
  }
  const Type *element = nullptr;
  for (size_t index = 0; index < types.size(); ++index) {
    if (types[index]->isError()) {
      return nullptr;
    }
    auto joined = this->checkTypesJoin(literal, types, element, types[index]);
    if (joined.isErr()) {
      this->state.diagnostics.error(literal->elements[index].expr->loc(), joined.error().message);
      return nullptr;
    }
    element = joined.value();
  }
  auto known = this->checkElementKnown(element);
  if (known.isErr()) {
    this->state.diagnostics.error(literal->loc(), known.error().message);
    return nullptr;
  }

  return element;
}

const Type *ArrayLiterals::tupleElement(const Type *tuple) const {
  const Type *element = nullptr;
  for (const Field &field : tuple->fields()) {
    element = element != nullptr ? this->state.types.literals.unionUnify(element, field.type)
                                 : field.type;
    if (element == nullptr || element->isError()) {
      return nullptr;
    }
  }

  return element;
}

}
