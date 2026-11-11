#pragma once

#include "compiler/result/result/result.h"
#include "compiler/semantic/bodies/values/array_literal_checker/array_literal_checker_errors.h"
#include "compiler/semantic/semantic_state/semantic_state.h"

namespace tz {

class SemanticParts;
class DictChecker;
class ExprChecker;
class FlowChecker;
class JsonChecker;
class MatchChecker;
class TupleChecker;
class TypeRelations;
class IteratorMethods;
class GeneratorChecker;

class ArrayLiteralChecker {
public:
  ArrayLiteralChecker(SemanticState &owner, SemanticParts &all);

  Expr *checkArrayLit(ArrayLitExpr *lit, const Type *expected);

private:
  using ResultElementGiven = Result<const Expr *, ArrayLiteralCheckerElementGivenError>;
  using ResultTupleElement = Result<const Type *, ArrayLiteralCheckerTupleSpreadError>;
  using ResultSpreadSource = Result<const Type *, ArrayLiteralCheckerSpreadableError>;
  using ResultArrayHasItem = Result<const ArrayLitExpr *, ArrayLiteralCheckerNotEmptyError>;
  using ResultElementsJoin = Result<const Type *, ArrayLiteralCheckerTypesJoinError>;
  using ResultElementKnown = Result<const Type *, ArrayLiteralCheckerElementKnownError>;
  using ResultSpreadFitsIn = Result<const Type *, ArrayLiteralCheckerSpreadFitsError>;

  static ResultElementGiven elementGiven(const ArrayLitExpr::Element &e);
  static ResultTupleElement tupleSpread(const Type *type);
  static ResultSpreadSource spreadable(const Type *type);
  static ResultArrayHasItem notEmpty(const ArrayLitExpr *lit);
  static ResultElementKnown elementKnown(const Type *element);
  static ResultSpreadFitsIn spreadFits(const Type *type, const Type *element,
                                       const Type *arrayType);
  static ResultElementsJoin typesJoin(const ArrayLitExpr *lit, llvm::ArrayRef<const Type *> types,
                                      const Type *element, const Type *type);

  struct ArrayElements {
    std::vector<const Type *> types;
    bool someDecimal;
    const Type *seen;
  };

  Expr *checkPairLiteral(ArrayLitExpr *lit);
  static const Type *wantedArray(const Type *expected);
  Expr *reportHoles(ArrayLitExpr *lit);
  Expr *checkTupleValue(ArrayLitExpr *lit, const Type *tupleType);
  static const Expr *firstSpread(const ArrayLitExpr *lit);
  void walkArrayElement(ArrayLitExpr *lit, size_t i, const Type *expectedElement,
                        ArrayElements &elements);
  void walkPlainElement(ArrayLitExpr::Element &e, size_t i, const Type *expectedElement,
                        ArrayElements &elements);
  void walkSpreadElement(ArrayLitExpr::Element &e, size_t i, const Type *expectedElement,
                         ArrayElements &elements);
  const Type *iteratedSpread(const Expr *spread);
  const Type *tupleSpreadType(const Expr *spread);
  const Type *arraySpreadType(const Expr *spread);
  static const Type *literalHint(const ArrayLitExpr *lit, const ArrayElements &elements);
  const Type *joinedElement(const ArrayLitExpr *lit, llvm::ArrayRef<const Type *> types);
  Expr *finishArrayLit(ArrayLitExpr *lit, const Type *element);

  SemanticState &state;
  DictChecker *dictChecker = nullptr;
  ExprChecker *exprChecker = nullptr;
  FlowChecker *flowChecker = nullptr;
  JsonChecker *jsonChecker = nullptr;
  MatchChecker *matchChecker = nullptr;
  TupleChecker *tupleChecker = nullptr;
  TypeRelations *typeRelations = nullptr;
  IteratorMethods *iteratorMethods = nullptr;
  GeneratorChecker *generatorChecker = nullptr;
};

}
