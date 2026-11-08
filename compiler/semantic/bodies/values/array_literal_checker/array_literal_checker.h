#pragma once

#include "compiler/result/result/result.h"
#include "compiler/semantic/bodies/values/array_literal_checker/array_literal_checker_errors.h"
#include "compiler/semantic/semantic_state/semantic_state.h"

namespace tz {

class ArrayLiteralChecker {
public:
  explicit ArrayLiteralChecker(SemanticState &owner);

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

  SemanticState &state;
};

}
