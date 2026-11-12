#pragma once

#include "compiler/result/result/result.h"
#include "compiler/semantic/bodies/values/array_literal_checker/array_literal_checker_errors.h"
#include "compiler/semantic/semantic_state/semantic_state.h"

namespace tz {

class Destination;
class SemanticParts;
class DictChecker;
class ExprChecker;
class FlowChecker;
class JsonChecker;
class MatchChecker;
class TupleChecker;
class TypeRelations;
class DecimalChecker;
class IteratorMethods;
class GeneratorChecker;

class ArrayLiteralChecker {
public:
  ArrayLiteralChecker(SemanticState &owner, SemanticParts &all);

  Expr *checkArrayLit(ArrayLitExpr *literal, const Type *expected);

private:
  using ResultElementGiven = Result<const Expr *, ArrayLiteralCheckerElementGivenError>;
  using ResultTupleElement = Result<const Type *, ArrayLiteralCheckerTupleSpreadError>;
  using ResultSpreadSource = Result<const Type *, ArrayLiteralCheckerSpreadableError>;
  using ResultArrayHasItem = Result<const ArrayLitExpr *, ArrayLiteralCheckerNotEmptyError>;
  using ResultElementsJoin = Result<const Type *, ArrayLiteralCheckerTypesJoinError>;
  using ResultElementKnown = Result<const Type *, ArrayLiteralCheckerElementKnownError>;
  using ResultSpreadFitsIn = Result<const Type *, ArrayLiteralCheckerSpreadFitsError>;

  struct ArrayElements {
    std::vector<const Type *> types;
    bool hasDecimal;
    const Type *seen;
  };

  ResultElementGiven checkElementGiven(const ArrayLitExpr::Element &element) const;
  ResultTupleElement checkTupleSpread(const Type *type) const;
  ResultSpreadSource checkSpreadable(const Type *type) const;
  ResultArrayHasItem checkNotEmpty(const ArrayLitExpr *literal) const;
  ResultElementsJoin checkTypesJoin(const ArrayLitExpr *literal, llvm::ArrayRef<const Type *> types,
                                    const Type *element, const Type *type) const;
  std::string joinHint(const ArrayLitExpr *literal, llvm::ArrayRef<const Type *> types,
                       const Type *element, const Type *type) const;
  bool isLiteralClass(const Type *type) const;
  bool hasSpread(const ArrayLitExpr *literal) const;
  bool hasFailedType(llvm::ArrayRef<const Type *> types) const;
  ResultElementKnown checkElementKnown(const Type *element) const;
  ResultSpreadFitsIn checkSpreadFits(const Type *type, const Type *element,
                                     const Type *arrayType) const;
  bool isSpreadFitting(const Type *type, const Type *element, const Type *arrayType) const;
  bool isFittingObjectArray(const Type *type, const Type *element) const;
  bool areFieldsConvertible(const Type *tuple, const Type *element) const;
  const Type *wantedElement(const Type *wanted) const;
  void walkUnseenElement(ArrayLitExpr *literal, size_t index, const Type *literalHint,
                         ArrayElements &elements);
  Expr *checkPairLiteral(ArrayLitExpr *literal);
  bool isBadPart(const Type *part) const;
  const Type *wantedArray(const Type *expected) const;
  Expr *reportHoles(ArrayLitExpr *literal);
  Expr *reportHole(ArrayLitExpr *literal, const ArrayLitExpr::Element &hole,
                   const std::string &message);
  Expr *checkTupleValue(ArrayLitExpr *literal, const Type *tupleType);
  const Expr *firstSpread(const ArrayLitExpr *literal) const;
  void walkArrayElement(ArrayLitExpr *literal, size_t index, const Type *expectedElement,
                        ArrayElements &elements);
  void walkPlainElement(ArrayLitExpr::Element &element, size_t index, const Type *expectedElement,
                        ArrayElements &elements);
  void walkSpreadElement(ArrayLitExpr::Element &element, size_t index, const Type *expectedElement,
                         ArrayElements &elements);
  const Type *seenJoined(const Type *seen, const Type *type) const;
  void addSpreadType(ArrayElements &elements, size_t index, const Type *type);
  const Type *iteratedSpread(const Expr *spread);
  const Type *tupleSpreadType(const Expr *spread);
  const Type *arraySpreadType(const Expr *spread);
  const Type *literalHint(const ArrayLitExpr *literal, const ArrayElements &elements) const;
  const Type *joinedElement(const ArrayLitExpr *literal, llvm::ArrayRef<const Type *> types);
  Expr *finishArrayLit(ArrayLitExpr *literal, const Type *element);
  void finishElement(ArrayLitExpr::Element &item, const Type *element, const Type *arrayType);

  SemanticState &state;
  DictChecker *dictChecker = nullptr;
  ExprChecker *exprChecker = nullptr;
  FlowChecker *flowChecker = nullptr;
  JsonChecker *jsonChecker = nullptr;
  Destination *destination = nullptr;
  MatchChecker *matchChecker = nullptr;
  TupleChecker *tupleChecker = nullptr;
  TypeRelations *typeRelations = nullptr;
  DecimalChecker *decimalChecker = nullptr;
  IteratorMethods *iteratorMethods = nullptr;
  GeneratorChecker *generatorChecker = nullptr;
};

}
