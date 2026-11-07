#pragma once

#include "compiler/check/checker/checker.h"
#include "compiler/check/iterators/iterable_checker/iterable_checker_errors.h"
#include "compiler/result/result/result.h"

namespace tz {

class IterableChecker {
public:
  using ResultIterableParam = Result<const Type *, IterableCheckerIterableParamError>;

  explicit IterableChecker(Checker &owner);

  Expr *userIterator(Expr *expr);
  Expr *asyncUserIterator(Expr *expr);
  const Type *asyncIterableType(const Type *type);
  Expr *iteratorOf(Expr *it);
  const Type *nextValueType(const Type *type);
  const Type *elementTypeOf(const Type *type);
  FuncSymbol *iterateFunction();
  static const Type *iteratorResultType(const Type *value, const Type *returned);
  ResultIterableParam iterableParam(FunctionDecl *fn, FuncSymbol *sym, const TypeRef &ref);
  bool isIterableRef(const TypeRef &ref) const;

private:
  using ResultIteratorGiven = Result<const Expr *, IterableCheckerIteratorGivenError>;
  using ResultAsyncIterator = Result<const Expr *, IterableCheckerAsyncIteratorGivenError>;

  static ResultIteratorGiven iteratorGiven(const Type *type, const Expr *result);
  static ResultAsyncIterator asyncIteratorGiven(const Type *type, const Expr *result);

  Checker &checker;
  FuncSymbol *iterateSymbol = nullptr;
};

}
