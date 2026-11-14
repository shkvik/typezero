#pragma once

#include "compiler/result/result/result.h"
#include "compiler/semantic/code/literals/code_literals_registry_errors.h"
#include "compiler/semantic/semantic_state/v2/semantic_state.h"
#include <string>

namespace tz {

class ArrayLiterals {
public:
  explicit ArrayLiterals(SemanticState &owner);

  void nextArrayElement(const NextArrayElement &task);
  void afterSpreadMatch(const AfterSpreadMatch &task);
  void checkArrayLiteral(const CheckArrayLiteral &task);
  void finishPairLiteral(const FinishPairLiteral &task);
  void afterSpreadElement(const AfterSpreadElement &task);
  void afterLiteralElement(const AfterLiteralElement &task);

private:
  using ResultElementsJoin = Result<const Type *, CodeLiteralsRegistryTypesJoinError>;
  using ResultSpreadFitsIn = Result<const Type *, CodeLiteralsRegistrySpreadFitsError>;
  using ResultSpreadSource = Result<const Type *, CodeLiteralsRegistrySpreadableError>;
  using ResultTupleElement = Result<const Type *, CodeLiteralsRegistryTupleSpreadError>;
  using ResultElementGiven = Result<const Expr *, CodeLiteralsRegistryElementGivenError>;
  using ResultElementKnown = Result<const Type *, CodeLiteralsRegistryElementKnownError>;
  using ResultArrayHasItem = Result<const ArrayLitExpr *, CodeLiteralsRegistryNotEmptyError>;

  void afterArrayPass(ArrayDraft draft);
  void finishSpreadElement(ArrayDraft draft);
  void pushLooseElements(ArrayLitExpr *literal);
  void startTupleLit(ArrayLitExpr *literal, const Type *tuple);
  void addSpreadType(ArrayDraft &draft, size_t index, const Type *type);


  bool isBadPart(const Type *part) const;
  bool isLiteralClass(const Type *type) const;
  bool hasSpread(const ArrayLitExpr *literal) const;
  bool hasFailedType(llvm::ArrayRef<const Type *> types) const;
  bool isFittingObjectArray(const Type *type, const Type *element) const;
  bool areFieldsConvertible(const Type *tuple, const Type *element) const;
  bool isSpreadFitting(const Type *type, const Type *element, const Type *arrayType) const;


  const Type *tupleSpreadType(const Expr *spread);
  const Type *arraySpreadType(const Expr *spread);
  const Type *tupleElement(const Type *tuple) const;
  const Type *wantedElement(const Type *wanted) const;
  const Type *wantedArray(const Type *expected) const;
  const Expr *firstSpread(const ArrayLitExpr *literal) const;
  const Type *seenJoined(const Type *seen, const Type *type) const;
  const Type *literalHint(const ArrayLitExpr *literal, const ArrayDraft &elements) const;
  const Type *joinedElement(const ArrayLitExpr *literal, llvm::ArrayRef<const Type *> types);


  ResultSpreadSource checkSpreadable(const Type *type) const;
  ResultTupleElement checkTupleSpread(const Type *type) const;
  ResultElementKnown checkElementKnown(const Type *element) const;
  ResultArrayHasItem checkNotEmpty(const ArrayLitExpr *literal) const;
  ResultElementGiven checkElementGiven(const ArrayLitExpr::Element &element) const;
  ResultSpreadFitsIn checkSpreadFits(const Type *type, const Type *element,
                                     const Type *arrayType) const;
  ResultElementsJoin checkTypesJoin(const ArrayLitExpr *literal, llvm::ArrayRef<const Type *> types,
                                    const Type *element, const Type *type) const;


  std::string joinHint(const ArrayLitExpr *literal, llvm::ArrayRef<const Type *> types,
                       const Type *element, const Type *type) const;

  SemanticState &state;
};

}
