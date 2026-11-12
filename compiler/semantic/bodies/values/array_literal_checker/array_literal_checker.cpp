#include "compiler/semantic/bodies/values/array_literal_checker/array_literal_checker.h"
#include "compiler/semantic/bodies/expr/expr_checker/expr_checker.h"
#include "compiler/semantic/bodies/iterators/generator_checker/generator_checker.h"
#include "compiler/semantic/bodies/iterators/iterator_methods/iterator_methods.h"
#include "compiler/semantic/bodies/objects/dict_checker/dict_checker.h"
#include "compiler/semantic/bodies/objects/tuple_checker/tuple_checker.h"
#include "compiler/semantic/flow/flow_checker/flow_checker.h"
#include "compiler/semantic/generics/generic_check_helpers/generic_check_helpers.h"
#include "compiler/semantic/library/decimal/decimal_checker/decimal_checker.h"
#include "compiler/semantic/library/json/json_checker/json_checker.h"
#include "compiler/semantic/library/regex/match_checker/match_checker.h"
#include "compiler/semantic/semantic_parts/semantic_parts.h"
#include "compiler/semantic/types/destination/destination.h"
#include "compiler/semantic/types/literal_types/literal_types.h"
#include "compiler/semantic/types/type_relations/type_relations.h"
#include "compiler/semantic/types/unknown_checker/unknown_checker.h"
#include "llvm/ADT/STLExtras.h"
#include <cstring>

namespace tz {

namespace {

constexpr const char *kTupleSeparator = ", ";

}

ArrayLiteralChecker::ArrayLiteralChecker(SemanticState &owner, SemanticParts &all)
    : state(owner) {
  this->dictChecker = &all.dictChecker;
  this->exprChecker = &all.exprChecker;
  this->flowChecker = &all.flowChecker;
  this->jsonChecker = &all.jsonChecker;
  this->destination = &all.destination;
  this->matchChecker = &all.matchChecker;
  this->tupleChecker = &all.tupleChecker;
  this->typeRelations = &all.typeRelations;
  this->decimalChecker = &all.decimalChecker;
  this->iteratorMethods = &all.iteratorMethods;
  this->generatorChecker = &all.generatorChecker;
}

ArrayLiteralChecker::ResultElementGiven
ArrayLiteralChecker::checkElementGiven(const ArrayLitExpr::Element &element) const {
  if (element.hole) {
    auto message = ErrorTextArrayHole::hole();

    return err(ArrayLiteralCheckerElementGivenError{
        .code = ArrayLiteralCheckerElementGivenErrorCode::HOLE_IN_ARRAY,
        .message = message,
    });
  }

  return ok(element.expr);
}

ArrayLiteralChecker::ResultTupleElement
ArrayLiteralChecker::checkTupleSpread(const Type *type) const {
  const auto *element = this->tupleChecker->tupleElement(type);
  if (element == nullptr) {
    auto message = ErrorTextTupleSpread::differ(type->name());

    return err(ArrayLiteralCheckerTupleSpreadError{
        .code = ArrayLiteralCheckerTupleSpreadErrorCode::ELEMENTS_DIFFER,
        .message = message,
    });
  }

  return ok(element);
}

ArrayLiteralChecker::ResultSpreadSource
ArrayLiteralChecker::checkSpreadable(const Type *type) const {
  if (!type->isArray() && !type->isError()) {
    auto message = ErrorTextSpreadable::notSpread(type->name(), type->isRecord());

    return err(ArrayLiteralCheckerSpreadableError{
        .code = ArrayLiteralCheckerSpreadableErrorCode::NOT_SPREADABLE,
        .message = message,
    });
  }

  return ok(type);
}

ArrayLiteralChecker::ResultArrayHasItem
ArrayLiteralChecker::checkNotEmpty(const ArrayLitExpr *literal) const {
  if (literal->elements.empty()) {
    auto message = ErrorTextArrayEmpty::empty();

    return err(ArrayLiteralCheckerNotEmptyError{
        .code = ArrayLiteralCheckerNotEmptyErrorCode::EMPTY_ARRAY,
        .message = message,
    });
  }

  return ok(literal);
}

ArrayLiteralChecker::ResultElementsJoin
ArrayLiteralChecker::checkTypesJoin(const ArrayLitExpr *literal, llvm::ArrayRef<const Type *> types,
                                    const Type *element, const Type *type) const {
  const Type *next = element != nullptr ? LiteralTypes::unionUnify(element, type) : type;
  if (next != nullptr) {
    return ok(next);
  }

  auto message = ErrorTextArrayTypes::differ(element->name(), type->name());
  message += this->joinHint(literal, types, element, type);

  return err(ArrayLiteralCheckerTypesJoinError{
      .code = ArrayLiteralCheckerTypesJoinErrorCode::ELEMENT_TYPES_DIFFER,
      .message = message,
  });
}

std::string ArrayLiteralChecker::joinHint(const ArrayLitExpr *literal,
                                          llvm::ArrayRef<const Type *> types, const Type *element,
                                          const Type *type) const {
  if (this->isLiteralClass(element) && this->isLiteralClass(type)) {
    return ErrorTextArrayTypes::literals();
  }
  if (this->hasSpread(literal) || this->hasFailedType(types)) {
    return "";
  }
  auto tuple = GenericCheckHelpers::typeNames(types, kTupleSeparator);

  return ErrorTextArrayTypes::tuple(tuple);
}

bool ArrayLiteralChecker::isLiteralClass(const Type *type) const {
  const ClassDecl *classDecl = type->nonOptional()->classDecl();

  return classDecl != nullptr && classDecl->literal() != nullptr;
}

bool ArrayLiteralChecker::hasSpread(const ArrayLitExpr *literal) const {
  for (const ArrayLitExpr::Element &element : literal->elements) {
    if (element.spread) {
      return true;
    }
  }

  return false;
}

bool ArrayLiteralChecker::hasFailedType(llvm::ArrayRef<const Type *> types) const {
  for (const Type *type : types) {
    if (type->isError()) {
      return true;
    }
  }

  return false;
}

ArrayLiteralChecker::ResultElementKnown
ArrayLiteralChecker::checkElementKnown(const Type *element) const {
  if (element->isUndefined() || element->isVoid()) {
    auto message = ErrorTextArrayElement::alone(element->name());

    return err(ArrayLiteralCheckerElementKnownError{
        .code = ArrayLiteralCheckerElementKnownErrorCode::ONLY_UNDEFINED,
        .message = message,
    });
  }

  return ok(element);
}

ArrayLiteralChecker::ResultSpreadFitsIn
ArrayLiteralChecker::checkSpreadFits(const Type *type, const Type *element,
                                     const Type *arrayType) const {
  if (!this->isSpreadFitting(type, element, arrayType)) {
    auto message = ErrorTextSpreadFits::into(type->name(), arrayType->name());

    return err(ArrayLiteralCheckerSpreadFitsError{
        .code = ArrayLiteralCheckerSpreadFitsErrorCode::SPREAD_MISMATCH,
        .message = message,
    });
  }

  return ok(type);
}

bool ArrayLiteralChecker::isSpreadFitting(const Type *type, const Type *element,
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
  if (type->isIterator() && TypeRelations::isImplicitlyConvertible(type->yielded(), element)) {
    return true;
  }
  if (type->isTypedArray() &&
      TypeRelations::isImplicitlyConvertible(type->elementType(), element)) {
    return true;
  }

  return type->isTuple() && this->areFieldsConvertible(type, element);
}

bool ArrayLiteralChecker::isFittingObjectArray(const Type *type, const Type *element) const {
  const Type *inner = type->inner();
  bool isObjects =
      UnknownChecker::isObjectReference(inner->nonOptional()) && element->nonOptional()->isObject();
  bool isOptionalFitting = inner->isOptional() == element->isOptional() || element->isOptional();

  return isObjects && isOptionalFitting && TypeRelations::isImplicitlyConvertible(inner, element);
}

bool ArrayLiteralChecker::areFieldsConvertible(const Type *tuple, const Type *element) const {
  for (const Field &field : tuple->fields()) {
    if (!TypeRelations::isImplicitlyConvertible(field.type, element)) {
      return false;
    }
  }

  return true;
}

Expr *ArrayLiteralChecker::checkArrayLit(ArrayLitExpr *literal, const Type *expected) {
  bool isPairLiteral = this->state.getExpressionMarks().isPairLiteral(literal);
  this->state.getExpressionMarks().forgetPairLiteral(literal);
  if (isPairLiteral && expected == nullptr) {
    return this->checkPairLiteral(literal);
  }
  if (expected != nullptr && expected->nonOptional()->isJson()) {
    return this->jsonChecker->checkJsonArrayLit(literal);
  }
  const auto *wanted = this->wantedArray(expected);
  const auto *expectedElement = this->wantedElement(wanted);
  auto *holes = this->reportHoles(literal);
  if (holes != nullptr) {
    return holes;
  }
  if (wanted != nullptr && wanted->nonOptional()->isTuple()) {
    return this->checkTupleValue(literal, wanted->nonOptional());
  }
  auto elements = ArrayElements{
      .types = std::vector<const Type *>(literal->elements.size()),
      .hasDecimal = false,
      .seen = nullptr,
  };
  for (size_t index = 0; index < literal->elements.size(); ++index) {
    this->walkArrayElement(literal, index, expectedElement, elements);
  }
  const auto *literalHint = this->literalHint(literal, elements);
  for (size_t index = 0; index < literal->elements.size(); ++index) {
    this->walkUnseenElement(literal, index, literalHint, elements);
  }
  literal->type = Type::error();
  const Type *element =
      expectedElement != nullptr ? expectedElement : this->joinedElement(literal, elements.types);
  if (element == nullptr || element->isError()) {
    return literal;
  }

  return this->finishArrayLit(literal, element);
}

const Type *ArrayLiteralChecker::wantedElement(const Type *wanted) const {
  if (wanted == nullptr || !wanted->nonOptional()->isArray()) {
    return nullptr;
  }

  return wanted->nonOptional()->inner();
}

void ArrayLiteralChecker::walkUnseenElement(ArrayLitExpr *literal, size_t index,
                                            const Type *literalHint, ArrayElements &elements) {
  if (elements.types[index] != nullptr) {
    return;
  }
  ArrayLitExpr::Element &element = literal->elements[index];
  element.expr = this->exprChecker->checkExpr(element.expr, literalHint);
  elements.types[index] = element.expr->type;
}

Expr *ArrayLiteralChecker::checkPairLiteral(ArrayLitExpr *literal) {
  auto parts = std::vector<const Type *>();
  bool isBad = false;
  for (ArrayLitExpr::Element &element : literal->elements) {
    element.expr = this->exprChecker->checkExpr(element.expr);
    const Type *part = element.expr->type;
    parts.push_back(part);
    isBad = isBad || this->isBadPart(part);
  }
  literal->type = isBad ? Type::error() : Type::tuple(parts);

  return literal;
}

bool ArrayLiteralChecker::isBadPart(const Type *part) const {
  return part->isError() || part->isVoid() || part->isUndefined();
}

const Type *ArrayLiteralChecker::wantedArray(const Type *expected) const {
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

Expr *ArrayLiteralChecker::reportHoles(ArrayLitExpr *literal) {
  for (const ArrayLitExpr::Element &hole : literal->elements) {
    auto given = this->checkElementGiven(hole);
    if (given.isErr()) {
      return this->reportHole(literal, hole, given.error().message);
    }
  }

  return nullptr;
}

Expr *ArrayLiteralChecker::reportHole(ArrayLitExpr *literal, const ArrayLitExpr::Element &hole,
                                      const std::string &message) {
  this->state.getDiagnostics().error(hole.expr->loc(), message);
  for (ArrayLitExpr::Element &element : literal->elements) {
    element.expr = this->exprChecker->checkExpr(element.expr);
  }
  literal->type = Type::error();

  return literal;
}

Expr *ArrayLiteralChecker::checkTupleValue(ArrayLitExpr *literal, const Type *tupleType) {
  auto tuple = this->tupleChecker->checkTupleLit(literal, tupleType);
  if (tuple.isOk()) {
    return tuple.value();
  }
  bool isSpread = tuple.error().code == TupleCheckerCheckTupleLitErrorCode::SPREAD_IN_TUPLE;
  llvm::SMLoc at = isSpread ? this->firstSpread(literal)->loc() : literal->loc();
  this->state.getDiagnostics().error(at, tuple.error().message);

  return literal;
}

const Expr *ArrayLiteralChecker::firstSpread(const ArrayLitExpr *literal) const {
  for (const ArrayLitExpr::Element &element : literal->elements) {
    if (element.spread) {
      return element.expr;
    }
  }

  return literal;
}

void ArrayLiteralChecker::walkArrayElement(ArrayLitExpr *literal, size_t index,
                                           const Type *expectedElement, ArrayElements &elements) {
  ArrayLitExpr::Element &element = literal->elements[index];
  bool isDecimalLater = !element.spread && expectedElement == nullptr &&
                        this->decimalChecker->isLiteralTree(element.expr);
  if (isDecimalLater) {
    return;
  }
  if (!element.spread) {
    return this->walkPlainElement(element, index, expectedElement, elements);
  }

  return this->walkSpreadElement(element, index, expectedElement, elements);
}

void ArrayLiteralChecker::walkPlainElement(ArrayLitExpr::Element &element, size_t index,
                                           const Type *expectedElement, ArrayElements &elements) {
  bool isShaped = expectedElement == nullptr && elements.seen != nullptr &&
                  elements.seen->holdsObjects() && this->destination->isLiteralShaped(element.expr);
  const Type *hint = isShaped ? elements.seen : expectedElement;
  element.expr = this->exprChecker->checkExpr(element.expr, hint);
  const Type *type = element.expr->type;
  elements.types[index] = type;
  elements.hasDecimal = elements.hasDecimal || type->nonOptional()->isDecimal();
  if (!type->isError() && elements.seen != Type::error()) {
    elements.seen = this->seenJoined(elements.seen, type);
  }
  if (elements.seen == nullptr) {
    elements.seen = Type::error();
  }
}

const Type *ArrayLiteralChecker::seenJoined(const Type *seen, const Type *type) const {
  if (seen == nullptr) {
    return type;
  }

  return this->typeRelations->unify(seen, type);
}

void ArrayLiteralChecker::walkSpreadElement(ArrayLitExpr::Element &element, size_t index,
                                            const Type *expectedElement, ArrayElements &elements) {
  this->iteratorMethods->markFused(element.expr);
  const Type *hint = expectedElement != nullptr ? Type::array(expectedElement) : nullptr;
  auto *checked = this->exprChecker->checkExpr(element.expr, hint);
  element.expr = this->dictChecker->iterableOf(checked);
  if (element.expr->type->isMatch()) {
    element.expr = this->matchChecker->matchItems(element.expr);
  }
  const Type *type = element.expr->type;
  if (type->isString()) {
    elements.types[index] = type;
    return;
  }
  if (type->isIterator()) {
    const auto *yielded = this->iteratedSpread(element.expr);
    return this->addSpreadType(elements, index, yielded);
  }
  if (type->isTuple()) {
    elements.types[index] = this->tupleSpreadType(element.expr);
    return;
  }
  if (type->isTypedArray()) {
    elements.types[index] = type->elementType();
    return;
  }

  const auto *spreadElement = this->arraySpreadType(element.expr);

  return this->addSpreadType(elements, index, spreadElement);
}

void ArrayLiteralChecker::addSpreadType(ArrayElements &elements, size_t index, const Type *type) {
  elements.types[index] = type;
  elements.hasDecimal = elements.hasDecimal || type->nonOptional()->isDecimal();
}

const Type *ArrayLiteralChecker::iteratedSpread(const Expr *spread) {
  const Type *type = spread->type;
  auto what = ErrorTextSpreadable::spread();
  auto nothing = this->generatorChecker->checkSendsNothing(type, what);
  if (nothing.isErr()) {
    this->state.getDiagnostics().error(spread->loc(), nothing.error().message);
  }
  this->flowChecker->calledFunction();

  return type->yielded();
}

const Type *ArrayLiteralChecker::tupleSpreadType(const Expr *spread) {
  auto element = this->checkTupleSpread(spread->type);
  if (element.isErr()) {
    this->state.getDiagnostics().error(spread->loc(), element.error().message);
    return Type::error();
  }

  return element.value();
}

const Type *ArrayLiteralChecker::arraySpreadType(const Expr *spread) {
  const Type *type = spread->type;
  auto spreadableTo = this->checkSpreadable(type);
  if (spreadableTo.isErr()) {
    this->state.getDiagnostics().error(spread->loc(), spreadableTo.error().message);
    return Type::error();
  }

  return type->isError() ? type : type->inner();
}

const Type *ArrayLiteralChecker::literalHint(const ArrayLitExpr *literal,
                                             const ArrayElements &elements) const {
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
    bool isFractional = elements.types[index] == nullptr &&
                        !this->typeRelations->isIntLiteralTree(literal->elements[index].expr);
    if (isFractional) {
      return Type::doubleTy();
    }
  }

  return nullptr;
}

const Type *ArrayLiteralChecker::joinedElement(const ArrayLitExpr *literal,
                                               llvm::ArrayRef<const Type *> types) {
  auto filled = this->checkNotEmpty(literal);
  if (filled.isErr()) {
    this->state.getDiagnostics().error(literal->loc(), filled.error().message);
    return nullptr;
  }
  const Type *element = nullptr;
  for (size_t index = 0; index < types.size(); ++index) {
    if (types[index]->isError()) {
      return nullptr;
    }
    auto joined = this->checkTypesJoin(literal, types, element, types[index]);
    if (joined.isErr()) {
      this->state.getDiagnostics().error(literal->elements[index].expr->loc(),
                                         joined.error().message);
      return nullptr;
    }
    element = joined.value();
  }
  auto known = this->checkElementKnown(element);
  if (known.isErr()) {
    this->state.getDiagnostics().error(literal->loc(), known.error().message);
    return nullptr;
  }

  return element;
}

Expr *ArrayLiteralChecker::finishArrayLit(ArrayLitExpr *literal, const Type *element) {
  const auto *arrayType = Type::array(element);
  for (ArrayLitExpr::Element &item : literal->elements) {
    this->finishElement(item, element, arrayType);
  }
  literal->type = arrayType;

  return literal;
}

void ArrayLiteralChecker::finishElement(ArrayLitExpr::Element &item, const Type *element,
                                        const Type *arrayType) {
  if (!item.spread) {
    item.expr = this->typeRelations->coerce(item.expr, element);
    return;
  }
  auto goes = this->checkSpreadFits(item.expr->type, element, arrayType);
  if (goes.isErr()) {
    this->state.getDiagnostics().error(item.expr->loc(), goes.error().message);
  }
}

}
