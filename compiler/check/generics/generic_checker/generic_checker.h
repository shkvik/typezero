#pragma once

#include "compiler/check/checker/checker.h"

namespace tz {

class GenericChecker {
public:
  explicit GenericChecker(Checker &checker);

  static const Type *apparentType(const Type *type);
  Expr *apparent(Expr *expr);
  FuncSymbol *genericNamed(const NameExpr *name) const;
  static const Type *literalShape(const Expr *expr);
  void noteForced(const Expr *expr, const Type *to);
  Expr *forceConversion(Expr *expr, const Type *to);
  std::optional<bool> constantCondition(const Expr *cond) const;
  const Type *narrowedType(const VarSymbol *var) const;
  const Type *numberParam(const Type *param) const;

private:
  Checker &checker_;
  mutable llvm::DenseMap<const Type *, const Type *> numberParams_;
};

}
