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

using llvm::cast;
using llvm::dyn_cast;
using llvm::dyn_cast_or_null;
using llvm::isa;

namespace tz {

ArrayLiteralChecker::ArrayLiteralChecker(Checker &checker) : checker_(checker) {}

Expr *ArrayLiteralChecker::checkArrayLit(ArrayLitExpr *lit, const Type *expected) {
  if (expected && expected->nonOptional()->isJson()) {
    return checker_.jsonChecker().checkJsonArrayLit(lit);
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

  if (auto hole =
          llvm::find_if(lit->elements, [](const ArrayLitExpr::Element &e) { return e.hole; });
      hole != lit->elements.end()) {
    checker_.diags_.error(hole->expr->loc(),
                          "arrays cannot have holes; write the element explicitly");
    for (ArrayLitExpr::Element &e : lit->elements) {
      e.expr = checker_.exprChecker().checkExpr(e.expr);
    }
    lit->type = Type::error();
    return lit;
  }
  if (expected && expected->nonOptional()->isTuple()) {
    return checker_.tupleChecker().checkTupleLit(lit, expected->nonOptional());
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
      e.expr = checker_.exprChecker().checkExpr(e.expr, hint);
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
    checker_.iteratorMethods().markFused(e.expr);
    e.expr = checker_.dictChecker().iterableOf(checker_.exprChecker().checkExpr(
        e.expr, expectedElement ? Type::array(expectedElement) : nullptr));
    if (e.expr->type->isMatch()) {
      e.expr = checker_.matchChecker().matchItems(e.expr);
    }
    const Type *type = e.expr->type;
    if (type->isString()) {
      types[i] = type;
      continue;
    }
    if (type->isIterator()) {
      checker_.generatorChecker().checkSendsNothing(type, e.expr->loc(), "a spread");
      checker_.flowChecker().calledFunction();
      types[i] = type->yielded();
      someDecimal |= types[i]->nonOptional()->isDecimal();
      continue;
    }
    if (type->isTuple()) {
      const Type *element = TupleChecker::tupleElement(type);
      if (!element) {
        checker_.diags_.error(
            e.expr->loc(), "the elements of '" + type->name() +
                               "' have different types, and an array holds elements of one type");
      }
      types[i] = element ? element : Type::error();
      continue;
    }
    if (type->isTypedArray()) {
      types[i] = type->elementType();
      continue;
    }
    if (!type->isArray() && !type->isError()) {
      checker_.diags_.error(e.expr->loc(),
                            "only arrays, strings, iterators, Maps and Sets can be spread "
                            "here, found '" +
                                type->name() + "'" +
                                (type->isRecord() ? "; for the entries of a Record, spread "
                                                    "'Object.entries(r)'"
                                                  : ""));
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
    e.expr = checker_.exprChecker().checkExpr(e.expr, literalHint);
    types[i] = e.expr->type;
  }

  lit->type = Type::error();
  const Type *element = expectedElement;
  if (!element) {
    if (lit->elements.empty()) {
      checker_.diags_.error(lit->loc(),
                            "cannot infer the element type of an empty array; add a type, "
                            "e.g. 'let a: int[] = []'");
      return lit;
    }
    for (size_t i = 0; i < types.size(); ++i) {
      if (types[i]->isError()) {
        return lit;
      }
      const Type *next = element ? LiteralTypes::unionUnify(element, types[i]) : types[i];
      if (!next) {
        std::string message = "array elements must have one type; found '" + element->name().str() +
                              "' and '" + types[i]->name().str() + "'";
        bool spreads =
            llvm::any_of(lit->elements, [](const ArrayLitExpr::Element &e) { return e.spread; });
        auto literalClass = [](const Type *t) {
          const ClassDecl *cls = t->nonOptional()->classDecl();

          return cls && cls->literal;
        };
        if (literalClass(element) && literalClass(types[i])) {
          message += "; each object literal with methods has a type of its own, so objects made "
                     "by different literals differ; an interface with their fields and methods "
                     "joins them: 'const xs: Counter[] = [...]'";
        } else if (!spreads && llvm::none_of(types, [](const Type *t) { return t->isError(); })) {
          std::string tuple;
          for (const Type *t : types) {
            tuple += (tuple.empty() ? "" : ", ") + t->name().str();
          }
          message += "; for a value of each type in its own place, declare a tuple, e.g. "
                     "'const t: [" +
                     tuple + "] = ...'";
        }
        checker_.diags_.error(lit->elements[i].expr->loc(), message);
        return lit;
      }
      element = next;
    }
    if (element->isUndefined() || element->isVoid()) {
      checker_.diags_.error(lit->loc(),
                            "cannot infer the element type from '" + element->name() +
                                "' alone; add a type, e.g. 'let a: (int | undefined)[] = ...'");
      return lit;
    }
  }
  if (element->isError()) {
    return lit;
  }

  const Type *arrayType = Type::array(element);
  for (ArrayLitExpr::Element &e : lit->elements) {
    if (!e.spread) {
      e.expr = checker_.typeRelations().coerce(e.expr, element);
    } else if (e.expr->type != arrayType && !e.expr->type->isError() &&
               !(e.expr->type->isString() && element->isString()) &&
               !(e.expr->type->isArray() &&
                 UnknownChecker::isObjectReference(e.expr->type->inner()->nonOptional()) &&
                 element->nonOptional()->isObject() &&
                 (e.expr->type->inner()->isOptional() == element->isOptional() ||
                  element->isOptional()) &&
                 TypeRelations::isImplicitlyConvertible(e.expr->type->inner(), element)) &&
               !(e.expr->type->isIterator() &&
                 TypeRelations::isImplicitlyConvertible(e.expr->type->yielded(), element)) &&
               !(e.expr->type->isTypedArray() &&
                 TypeRelations::isImplicitlyConvertible(e.expr->type->elementType(), element)) &&
               !(e.expr->type->isTuple() &&
                 llvm::all_of(e.expr->type->fields(), [&](const Field &f) {
                   return TypeRelations::isImplicitlyConvertible(f.type, element);
                 }))) {
      checker_.diags_.error(e.expr->loc(), "cannot spread '" + e.expr->type->name() + "' into '" +
                                               arrayType->name() + "'");
    }
  }
  lit->type = arrayType;

  return lit;
}

}
