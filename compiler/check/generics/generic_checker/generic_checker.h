#pragma once

#include "compiler/check/sema/sema.h"

namespace tz {

// Values of type parameters: their apparent types and the conversions they force.
class GenericChecker {
public:
  explicit GenericChecker(Sema &sema);

  // The type whose operations a value of `type` has: the constraint of a
  // type parameter, `T extends number` is a number; `type` itself otherwise.
  static const Type *apparentType(const Type *type);
  // `expr` converted to its apparent type, so that `x * 2` with `x` of type
  // `T extends number` multiplies doubles.
  Expr *apparent(Expr *expr);
  FuncSymbol *genericNamed(const NameExpr *name) const;
  // The type an argument written as a literal (`1`, `[1, 2.5]`, `{ x: 1 }`)
  // has without an expected type, found before it is checked: it is checked
  // with the type its parameter gets, as `pair(1, 2.5)` makes 1 a double.
  // Null for other expressions.
  static const Type *literalShape(const Expr *expr);
  // Generic code decides how a value of a type parameter is converted (to
  // its constraint for arithmetic, to a string next to one); an instance
  // converts the value of the same expression the same way, whatever its
  // type there.
  void noteForced(const Expr *expr, const Type *to);
  Expr *forceConversion(Expr *expr, const Type *to);
  // In an instance: the value of a condition that its types decide, such as
  // `typeof x == "string"` with x a number. The branch that cannot run is
  // dropped unchecked: it was written for other types.
  std::optional<bool> constantCondition(const Expr *cond) const;
  const Type *narrowedType(const VarSymbol *var) const;
  // `typeof x == "number"` for x of type parameter `param`: a type parameter
  // that is a number (a double for an unconstrained one).
  const Type *numberParam(const Type *param) const;

private:
  Sema &sema_;
  mutable llvm::DenseMap<const Type *, const Type *> numberParams_;
};

} // namespace tz
