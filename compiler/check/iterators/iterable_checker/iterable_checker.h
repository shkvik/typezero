#pragma once

#include "compiler/check/checker/checker.h"

namespace tz {

class IterableChecker {
public:
  explicit IterableChecker(Checker &checker);

  Expr *userIterator(Expr *expr);
  Expr *asyncUserIterator(Expr *expr);
  const Type *asyncIterableType(const Type *type);
  Expr *iteratorOf(Expr *it);
  const Type *nextValueType(const Type *type);
  const Type *elementTypeOf(const Type *type);
  FuncSymbol *iterateFunction();
  static const Type *iteratorResultType(const Type *value, const Type *returned);
  const Type *iterableParam(FunctionDecl *fn, FuncSymbol *sym, const TypeRef &ref);
  bool isIterableRef(const TypeRef &ref) const;

private:
  Checker &checker_;
  FuncSymbol *iterate_ = nullptr;
};

}
