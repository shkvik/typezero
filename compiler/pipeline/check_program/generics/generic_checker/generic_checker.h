#pragma once

#include "compiler/pipeline/check_program/check_state/check_state.h"

namespace tz {

class GenericChecker {
public:
  explicit GenericChecker(CheckState &owner);

  static const Type *apparentType(const Type *type);
  Expr *apparent(Expr *expr);
  FuncSymbol *genericNamed(const NameExpr *name) const;
  static const Type *literalShape(const Expr *expr);
  void noteForced(const Expr *expr, const Type *to);
  Expr *forceConversion(Expr *expr, const Type *to);
  std::optional<bool> constantCondition(const Expr *cond) const;
  const Type *narrowedType(const VarSymbol *var) const;
  const Type *numberParam(const Type *param, const Type *only = nullptr) const;

private:
  CheckState &state;
};

}
