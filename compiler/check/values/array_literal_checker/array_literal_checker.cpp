#include "compiler/check/values/array_literal_checker/array_literal_checker.h"
#include "compiler/check/expr/expr_checker/expr_checker.h"
#include "compiler/check/flow/flow_checker/flow_checker.h"
#include "compiler/check/iterators/generator_checker/generator_checker.h"
#include "compiler/check/iterators/iterator_methods/iterator_methods.h"
#include "compiler/check/objects/dict_checker/dict_checker.h"
#include "compiler/check/objects/tuple_checker/tuple_checker.h"
#include "compiler/check/types/destination/destination.h"
#include "compiler/check/types/literal_types/literal_types.h"
#include "compiler/check/types/type_relations/type_relations.h"
#include "compiler/check/types/unknown_checker/unknown_checker.h"
#include "compiler/library/decimal/decimal_checker/decimal_checker.h"
#include "compiler/library/json/json_checker/json_checker.h"
#include "compiler/library/regex/match_checker/match_checker.h"
#include "compiler/whole_program/flows/flows.h"
#include "llvm/ADT/STLExtras.h"
#include <cstring>


namespace tz {

ArrayLiteralChecker::ArrayLiteralChecker(Checker &owner)
    : checker(owner) {}

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
  if (this->checker.pairLiterals_.erase(lit) && !expected) {
    std::vector<const Type *> parts;
    for (ArrayLitExpr::Element &e : lit->elements) {
      e.expr = this->checker.exprChecker().checkExpr(e.expr);
      parts.push_back(e.expr->type);
    }
    bool bad = llvm::any_of(
        parts, [](const Type *t) { return t->isError() || t->isVoid() || t->isUndefined(); });
    lit->type = bad ? Type::error() : Type::tuple(parts);
    return lit;
  }
  if (expected && expected->nonOptional()->isJson()) {
    return this->checker.jsonChecker().checkJsonArrayLit(lit);
  }
  if (expected && expected->isUnknown()) {
    expected = Type::array(Type::unknown());
  }
  const Type *expectedElement = nullptr;
  if (expected && expected->nonOptional()->isUnion()) {
    if (const Type *array = expected->nonOptional()->slotPart(UnionSlot::Array);
        array && array->isArray()) {
      expected = array;
    }
  }
  if (expected && expected->nonOptional()->isArray()) {
    expectedElement = expected->nonOptional()->inner();
  }

  for (const ArrayLitExpr::Element &hole : lit->elements) {
    ResultElementGiven given = ArrayLiteralChecker::elementGiven(hole);
    if (given.isErr()) {
      this->checker.diags_.error(hole.expr->loc(), given.error().message);
      for (ArrayLitExpr::Element &e : lit->elements) {
        e.expr = this->checker.exprChecker().checkExpr(e.expr);
      }
      lit->type = Type::error();
      return lit;
    }
  }
  if (expected && expected->nonOptional()->isTuple()) {
    TupleChecker::ResultTupleFilled tuple =
        this->checker.tupleChecker().checkTupleLit(lit, expected->nonOptional());
    if (tuple.isOk()) {
      return tuple.value();
    }
    llvm::SMLoc at = lit->loc();
    auto spreads = [](const ArrayLitExpr::Element &e) { return e.spread; };
    switch (tuple.error().code) {
      case TupleCheckerCheckTupleLitErrorCode::SPREAD_IN_TUPLE:
        at = llvm::find_if(lit->elements, spreads)->expr->loc();
        break;
      case TupleCheckerCheckTupleLitErrorCode::WRONG_LENGTH:
        break;
    }
    this->checker.diags_.error(at, tuple.error().message);
    return lit;
  }

  std::vector<const Type *> types(lit->elements.size());
  bool someDecimal = false;
  const Type *seen = nullptr;
  for (size_t i = 0; i < lit->elements.size(); ++i) {
    ArrayLitExpr::Element &e = lit->elements[i];
    if (!e.spread && !expectedElement && DecimalChecker::isLiteralTree(e.expr)) {
      continue;
    }
    if (!e.spread) {
      const Type *hint = expectedElement;
      if (!hint && seen && Checker::holdsObjects(seen) && Destination::literalShaped(e.expr)) {
        hint = seen;
      }
      e.expr = this->checker.exprChecker().checkExpr(e.expr, hint);
      types[i] = e.expr->type;
      someDecimal |= types[i]->nonOptional()->isDecimal();
      if (!e.expr->type->isError() && seen != Type::error()) {
        seen = seen ? TypeRelations::unify(seen, e.expr->type) : e.expr->type;
      }
      if (!seen) {
        seen = Type::error();
      }
      continue;
    }
    this->checker.iteratorMethods().markFused(e.expr);
    e.expr = this->checker.dictChecker().iterableOf(this->checker.exprChecker().checkExpr(
        e.expr, expectedElement ? Type::array(expectedElement) : nullptr));
    if (e.expr->type->isMatch()) {
      e.expr = this->checker.matchChecker().matchItems(e.expr);
    }
    const Type *type = e.expr->type;
    if (type->isString()) {
      types[i] = type;
      continue;
    }
    if (type->isIterator()) {
      this->checker.generatorChecker().checkSendsNothing(type, e.expr->loc(), "a spread");
      this->checker.flowChecker().calledFunction();
      types[i] = type->yielded();
      someDecimal |= types[i]->nonOptional()->isDecimal();
      continue;
    }
    if (type->isTuple()) {
      ResultTupleElement element = ArrayLiteralChecker::tupleSpread(type);
      if (element.isErr()) {
        this->checker.diags_.error(e.expr->loc(), element.error().message);
      }
      types[i] = element.isOk() ? element.value() : Type::error();
      continue;
    }
    if (type->isTypedArray()) {
      types[i] = type->elementType();
      continue;
    }
    ResultSpreadSource spreadableTo = ArrayLiteralChecker::spreadable(type);
    if (spreadableTo.isErr()) {
      this->checker.diags_.error(e.expr->loc(), spreadableTo.error().message);
      type = Type::error();
    }
    types[i] = type->isError() ? type : type->inner();
    someDecimal |= types[i]->nonOptional()->isDecimal();
  }
  const Type *literalHint = someDecimal ? Type::decimalTy() : nullptr;
  if (!literalHint && seen && seen->isNumeric()) {
    literalHint = seen;
  }
  for (size_t i = 0; i < lit->elements.size() && !literalHint; ++i) {
    if (lit->elements[i].spread && types[i] && types[i]->isNumeric()) {
      literalHint = types[i];
    }
  }
  if (!literalHint) {
    for (size_t i = 0; i < lit->elements.size(); ++i) {
      if (!types[i] && !TypeRelations::isIntLiteralTree(lit->elements[i].expr)) {
        literalHint = Type::doubleTy();
      }
    }
  }
  for (size_t i = 0; i < lit->elements.size(); ++i) {
    ArrayLitExpr::Element &e = lit->elements[i];
    if (types[i]) {
      continue;
    }
    e.expr = this->checker.exprChecker().checkExpr(e.expr, literalHint);
    types[i] = e.expr->type;
  }

  lit->type = Type::error();
  const Type *element = expectedElement;
  if (!element) {
    ResultArrayHasItem filled = ArrayLiteralChecker::notEmpty(lit);
    if (filled.isErr()) {
      this->checker.diags_.error(lit->loc(), filled.error().message);
      return lit;
    }
    for (size_t i = 0; i < types.size(); ++i) {
      if (types[i]->isError()) {
        return lit;
      }
      ResultElementsJoin joined = ArrayLiteralChecker::typesJoin(lit, types, element, types[i]);
      if (joined.isErr()) {
        this->checker.diags_.error(lit->elements[i].expr->loc(), joined.error().message);
        return lit;
      }
      const Type *next = joined.value();
      element = next;
    }
    ResultElementKnown known = ArrayLiteralChecker::elementKnown(element);
    if (known.isErr()) {
      this->checker.diags_.error(lit->loc(), known.error().message);
      return lit;
    }
  }
  if (element->isError()) {
    return lit;
  }

  const Type *arrayType = Type::array(element);
  for (ArrayLitExpr::Element &e : lit->elements) {
    if (!e.spread) {
      e.expr = this->checker.typeRelations().coerce(e.expr, element);
      continue;
    }
    ResultSpreadFitsIn goes = ArrayLiteralChecker::spreadFits(e.expr->type, element, arrayType);
    if (goes.isErr()) {
      this->checker.diags_.error(e.expr->loc(), goes.error().message);
    }
  }
  lit->type = arrayType;

  return lit;
}

}
