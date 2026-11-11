#include "compiler/semantic/bodies/values/array_literal_checker/array_literal_checker.h"
#include "compiler/semantic/bodies/expr/expr_checker/expr_checker.h"
#include "compiler/semantic/bodies/iterators/generator_checker/generator_checker.h"
#include "compiler/semantic/bodies/iterators/iterator_methods/iterator_methods.h"
#include "compiler/semantic/bodies/objects/dict_checker/dict_checker.h"
#include "compiler/semantic/bodies/objects/tuple_checker/tuple_checker.h"
#include "compiler/semantic/flow/flow_checker/flow_checker.h"
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

ArrayLiteralChecker::ArrayLiteralChecker(SemanticState &owner, SemanticParts &all)
    : state(owner) {
  this->dictChecker = &all.dictChecker;
  this->exprChecker = &all.exprChecker;
  this->flowChecker = &all.flowChecker;
  this->jsonChecker = &all.jsonChecker;
  this->matchChecker = &all.matchChecker;
  this->tupleChecker = &all.tupleChecker;
  this->typeRelations = &all.typeRelations;
  this->iteratorMethods = &all.iteratorMethods;
  this->generatorChecker = &all.generatorChecker;
}

ArrayLiteralChecker::ResultElementGiven
ArrayLiteralChecker::elementGiven(const ArrayLitExpr::Element &e) {
  if (e.hole) {
    std::string message = ErrorTextArrayHole::hole();

    return err(ArrayLiteralCheckerElementGivenError{
        .code = ArrayLiteralCheckerElementGivenErrorCode::HOLE_IN_ARRAY,
        .message = message,
    });
  }

  return ok(e.expr);
}

ArrayLiteralChecker::ResultTupleElement ArrayLiteralChecker::tupleSpread(const Type *type) {
  const Type *element = TupleChecker::tupleElement(type);
  if (!element) {
    std::string message = ErrorTextTupleSpread::differ(type->name());

    return err(ArrayLiteralCheckerTupleSpreadError{
        .code = ArrayLiteralCheckerTupleSpreadErrorCode::ELEMENTS_DIFFER,
        .message = message,
    });
  }

  return ok(element);
}

ArrayLiteralChecker::ResultSpreadSource ArrayLiteralChecker::spreadable(const Type *type) {
  if (!type->isArray() && !type->isError()) {
    std::string message = ErrorTextSpreadable::notSpread(type->name(), type->isRecord());

    return err(ArrayLiteralCheckerSpreadableError{
        .code = ArrayLiteralCheckerSpreadableErrorCode::NOT_SPREADABLE,
        .message = message,
    });
  }

  return ok(type);
}

ArrayLiteralChecker::ResultArrayHasItem ArrayLiteralChecker::notEmpty(const ArrayLitExpr *lit) {
  if (lit->elements.empty()) {
    std::string message = ErrorTextArrayEmpty::empty();

    return err(ArrayLiteralCheckerNotEmptyError{
        .code = ArrayLiteralCheckerNotEmptyErrorCode::EMPTY_ARRAY,
        .message = message,
    });
  }

  return ok(lit);
}

ArrayLiteralChecker::ResultElementsJoin
ArrayLiteralChecker::typesJoin(const ArrayLitExpr *lit, llvm::ArrayRef<const Type *> types,
                               const Type *element, const Type *type) {
  const Type *next = element ? LiteralTypes::unionUnify(element, type) : type;
  if (!next) {
    std::string message = ErrorTextArrayTypes::differ(element->name(), type->name());
    auto spread = [](const ArrayLitExpr::Element &e) { return e.spread; };
    auto literalClass = [](const Type *t) {
      const ClassDecl *cls = t->nonOptional()->classDecl();

      return cls && cls->literal();
    };
    auto failed = [](const Type *t) { return t->isError(); };
    bool spreads = llvm::any_of(lit->elements, spread);
    if (literalClass(element) && literalClass(type)) {
      message += ErrorTextArrayTypes::literals();
    } else if (!spreads && llvm::none_of(types, failed)) {
      std::string tuple;
      for (const Type *t : types) {
        tuple += (tuple.empty() ? "" : ", ") + t->name().str();
      }
      message += ErrorTextArrayTypes::tuple(tuple);
    }

    return err(ArrayLiteralCheckerTypesJoinError{
        .code = ArrayLiteralCheckerTypesJoinErrorCode::ELEMENT_TYPES_DIFFER,
        .message = message,
    });
  }

  return ok(next);
}

ArrayLiteralChecker::ResultElementKnown ArrayLiteralChecker::elementKnown(const Type *element) {
  if (element->isUndefined() || element->isVoid()) {
    std::string message = ErrorTextArrayElement::alone(element->name());

    return err(ArrayLiteralCheckerElementKnownError{
        .code = ArrayLiteralCheckerElementKnownErrorCode::ONLY_UNDEFINED,
        .message = message,
    });
  }

  return ok(element);
}

ArrayLiteralChecker::ResultSpreadFitsIn
ArrayLiteralChecker::spreadFits(const Type *type, const Type *element, const Type *arrayType) {
  auto convertible = [&](const Field &f) {
    return TypeRelations::isImplicitlyConvertible(f.type, element);
  };
  bool fits =
      type == arrayType || type->isError() || (type->isString() && element->isString()) ||
      (type->isArray() && UnknownChecker::isObjectReference(type->inner()->nonOptional()) &&
       element->nonOptional()->isObject() &&
       (type->inner()->isOptional() == element->isOptional() || element->isOptional()) &&
       TypeRelations::isImplicitlyConvertible(type->inner(), element)) ||
      (type->isIterator() && TypeRelations::isImplicitlyConvertible(type->yielded(), element)) ||
      (type->isTypedArray() &&
       TypeRelations::isImplicitlyConvertible(type->elementType(), element)) ||
      (type->isTuple() && llvm::all_of(type->fields(), convertible));
  if (!fits) {
    std::string message = ErrorTextSpreadFits::into(type->name(), arrayType->name());

    return err(ArrayLiteralCheckerSpreadFitsError{
        .code = ArrayLiteralCheckerSpreadFitsErrorCode::SPREAD_MISMATCH,
        .message = message,
    });
  }

  return ok(type);
}

Expr *ArrayLiteralChecker::checkArrayLit(ArrayLitExpr *lit, const Type *expected) {
  bool pairLiteral = this->state.getExpressionMarks().isPairLiteral(lit);
  this->state.getExpressionMarks().forgetPairLiteral(lit);
  if (pairLiteral && expected == nullptr) {
    return this->checkPairLiteral(lit);
  }
  if (expected != nullptr && expected->nonOptional()->isJson()) {
    return this->jsonChecker->checkJsonArrayLit(lit);
  }
  const Type *wanted = ArrayLiteralChecker::wantedArray(expected);
  const Type *expectedElement = wanted != nullptr && wanted->nonOptional()->isArray()
                                    ? wanted->nonOptional()->inner()
                                    : nullptr;
  Expr *holes = this->reportHoles(lit);
  if (holes != nullptr) {
    return holes;
  }
  if (wanted != nullptr && wanted->nonOptional()->isTuple()) {
    return this->checkTupleValue(lit, wanted->nonOptional());
  }
  ArrayElements elements{
      .types = std::vector<const Type *>(lit->elements.size()),
      .someDecimal = false,
      .seen = nullptr,
  };
  for (size_t i = 0; i < lit->elements.size(); ++i) {
    this->walkArrayElement(lit, i, expectedElement, elements);
  }
  const Type *literalHint = ArrayLiteralChecker::literalHint(lit, elements);
  for (size_t i = 0; i < lit->elements.size(); ++i) {
    if (elements.types[i] != nullptr) {
      continue;
    }
    ArrayLitExpr::Element &e = lit->elements[i];
    e.expr = this->exprChecker->checkExpr(e.expr, literalHint);
    elements.types[i] = e.expr->type;
  }
  lit->type = Type::error();
  const Type *element =
      expectedElement != nullptr ? expectedElement : this->joinedElement(lit, elements.types);
  if (element == nullptr || element->isError()) {
    return lit;
  }

  return this->finishArrayLit(lit, element);
}

Expr *ArrayLiteralChecker::checkPairLiteral(ArrayLitExpr *lit) {
  std::vector<const Type *> parts;
  bool bad = false;
  for (ArrayLitExpr::Element &e : lit->elements) {
    e.expr = this->exprChecker->checkExpr(e.expr);
    const Type *part = e.expr->type;
    parts.push_back(part);
    bad |= part->isError() || part->isVoid() || part->isUndefined();
  }
  lit->type = bad ? Type::error() : Type::tuple(parts);

  return lit;
}

const Type *ArrayLiteralChecker::wantedArray(const Type *expected) {
  if (expected == nullptr) {
    return nullptr;
  }
  if (expected->isUnknown()) {
    return Type::array(Type::unknown());
  }
  if (!expected->nonOptional()->isUnion()) {
    return expected;
  }
  const Type *array = expected->nonOptional()->slotPart(UnionSlot::Array);
  if (array != nullptr && array->isArray()) {
    return array;
  }

  return expected;
}

Expr *ArrayLiteralChecker::reportHoles(ArrayLitExpr *lit) {
  for (const ArrayLitExpr::Element &hole : lit->elements) {
    ResultElementGiven given = ArrayLiteralChecker::elementGiven(hole);
    if (given.isOk()) {
      continue;
    }
    this->state.getDiagnostics().error(hole.expr->loc(), given.error().message);
    for (ArrayLitExpr::Element &e : lit->elements) {
      e.expr = this->exprChecker->checkExpr(e.expr);
    }
    lit->type = Type::error();
    return lit;
  }

  return nullptr;
}

Expr *ArrayLiteralChecker::checkTupleValue(ArrayLitExpr *lit, const Type *tupleType) {
  TupleChecker::ResultTupleFilled tuple = this->tupleChecker->checkTupleLit(lit, tupleType);
  if (tuple.isOk()) {
    return tuple.value();
  }
  bool spread = tuple.error().code == TupleCheckerCheckTupleLitErrorCode::SPREAD_IN_TUPLE;
  llvm::SMLoc at = spread ? ArrayLiteralChecker::firstSpread(lit)->loc() : lit->loc();
  this->state.getDiagnostics().error(at, tuple.error().message);

  return lit;
}

const Expr *ArrayLiteralChecker::firstSpread(const ArrayLitExpr *lit) {
  for (const ArrayLitExpr::Element &e : lit->elements) {
    if (e.spread) {
      return e.expr;
    }
  }

  return lit;
}

void ArrayLiteralChecker::walkArrayElement(ArrayLitExpr *lit, size_t i, const Type *expectedElement,
                                           ArrayElements &elements) {
  ArrayLitExpr::Element &e = lit->elements[i];
  if (!e.spread && expectedElement == nullptr && DecimalChecker::isLiteralTree(e.expr)) {
    return;
  }
  if (!e.spread) {
    return this->walkPlainElement(e, i, expectedElement, elements);
  }

  return this->walkSpreadElement(e, i, expectedElement, elements);
}

void ArrayLiteralChecker::walkPlainElement(ArrayLitExpr::Element &e, size_t i,
                                           const Type *expectedElement, ArrayElements &elements) {
  const Type *hint = expectedElement;
  bool shaped = hint == nullptr && elements.seen != nullptr && elements.seen->holdsObjects() &&
                Destination::literalShaped(e.expr);
  if (shaped) {
    hint = elements.seen;
  }
  e.expr = this->exprChecker->checkExpr(e.expr, hint);
  const Type *type = e.expr->type;
  elements.types[i] = type;
  elements.someDecimal |= type->nonOptional()->isDecimal();
  if (!type->isError() && elements.seen != Type::error()) {
    elements.seen = elements.seen != nullptr ? TypeRelations::unify(elements.seen, type) : type;
  }
  if (elements.seen == nullptr) {
    elements.seen = Type::error();
  }
}

void ArrayLiteralChecker::walkSpreadElement(ArrayLitExpr::Element &e, size_t i,
                                            const Type *expectedElement, ArrayElements &elements) {
  this->iteratorMethods->markFused(e.expr);
  const Type *hint = expectedElement != nullptr ? Type::array(expectedElement) : nullptr;
  Expr *checked = this->exprChecker->checkExpr(e.expr, hint);
  e.expr = this->dictChecker->iterableOf(checked);
  if (e.expr->type->isMatch()) {
    e.expr = this->matchChecker->matchItems(e.expr);
  }
  const Type *type = e.expr->type;
  if (type->isString()) {
    elements.types[i] = type;
    return;
  }
  if (type->isIterator()) {
    elements.types[i] = this->iteratedSpread(e.expr);
    elements.someDecimal |= elements.types[i]->nonOptional()->isDecimal();
    return;
  }
  if (type->isTuple()) {
    elements.types[i] = this->tupleSpreadType(e.expr);
    return;
  }
  if (type->isTypedArray()) {
    elements.types[i] = type->elementType();
    return;
  }
  elements.types[i] = this->arraySpreadType(e.expr);
  elements.someDecimal |= elements.types[i]->nonOptional()->isDecimal();
}

const Type *ArrayLiteralChecker::iteratedSpread(const Expr *spread) {
  const Type *type = spread->type;
  std::string what = ErrorTextSpreadable::spread();
  GeneratorChecker::ResultNextNeedsNoValue nothing =
      this->generatorChecker->checkSendsNothing(type, what);
  if (nothing.isErr()) {
    this->state.getDiagnostics().error(spread->loc(), nothing.error().message);
  }
  this->flowChecker->calledFunction();

  return type->yielded();
}

const Type *ArrayLiteralChecker::tupleSpreadType(const Expr *spread) {
  ResultTupleElement element = ArrayLiteralChecker::tupleSpread(spread->type);
  if (element.isErr()) {
    this->state.getDiagnostics().error(spread->loc(), element.error().message);
    return Type::error();
  }

  return element.value();
}

const Type *ArrayLiteralChecker::arraySpreadType(const Expr *spread) {
  const Type *type = spread->type;
  ResultSpreadSource spreadableTo = ArrayLiteralChecker::spreadable(type);
  if (spreadableTo.isErr()) {
    this->state.getDiagnostics().error(spread->loc(), spreadableTo.error().message);
    return Type::error();
  }

  return type->isError() ? type : type->inner();
}

const Type *ArrayLiteralChecker::literalHint(const ArrayLitExpr *lit,
                                             const ArrayElements &elements) {
  if (elements.someDecimal) {
    return Type::decimalTy();
  }
  if (elements.seen != nullptr && elements.seen->isNumeric()) {
    return elements.seen;
  }
  for (size_t i = 0; i < lit->elements.size(); ++i) {
    const Type *type = elements.types[i];
    if (lit->elements[i].spread && type != nullptr && type->isNumeric()) {
      return type;
    }
  }
  for (size_t i = 0; i < lit->elements.size(); ++i) {
    bool fractional =
        elements.types[i] == nullptr && !TypeRelations::isIntLiteralTree(lit->elements[i].expr);
    if (fractional) {
      return Type::doubleTy();
    }
  }

  return nullptr;
}

const Type *ArrayLiteralChecker::joinedElement(const ArrayLitExpr *lit,
                                               llvm::ArrayRef<const Type *> types) {
  ResultArrayHasItem filled = ArrayLiteralChecker::notEmpty(lit);
  if (filled.isErr()) {
    this->state.getDiagnostics().error(lit->loc(), filled.error().message);
    return nullptr;
  }
  const Type *element = nullptr;
  for (size_t i = 0; i < types.size(); ++i) {
    if (types[i]->isError()) {
      return nullptr;
    }
    ResultElementsJoin joined = ArrayLiteralChecker::typesJoin(lit, types, element, types[i]);
    if (joined.isErr()) {
      this->state.getDiagnostics().error(lit->elements[i].expr->loc(), joined.error().message);
      return nullptr;
    }
    element = joined.value();
  }
  ResultElementKnown known = ArrayLiteralChecker::elementKnown(element);
  if (known.isErr()) {
    this->state.getDiagnostics().error(lit->loc(), known.error().message);
    return nullptr;
  }

  return element;
}

Expr *ArrayLiteralChecker::finishArrayLit(ArrayLitExpr *lit, const Type *element) {
  const Type *arrayType = Type::array(element);
  for (ArrayLitExpr::Element &e : lit->elements) {
    if (!e.spread) {
      e.expr = this->typeRelations->coerce(e.expr, element);
      continue;
    }
    ResultSpreadFitsIn goes = ArrayLiteralChecker::spreadFits(e.expr->type, element, arrayType);
    if (goes.isErr()) {
      this->state.getDiagnostics().error(e.expr->loc(), goes.error().message);
    }
  }
  lit->type = arrayType;

  return lit;
}

}
