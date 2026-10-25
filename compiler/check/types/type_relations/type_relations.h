#pragma once

#include "compiler/check/sema/sema.h"

namespace tz {

// What converts to what: implicit conversions, the common type of two, coercions.
class TypeRelations {
public:
  explicit TypeRelations(Sema &sema);

  // Wraps `expr` in an implicit conversion to `to` (no checks).
  Expr *convert(Expr *expr, const Type *to);
  // Converts `expr` to `to` if that is allowed implicitly, else reports an
  // error; `what` names the place, as "field 'x'".
  Expr *coerce(Expr *expr, const Type *to, llvm::StringRef what = {});
  static bool isImplicitlyConvertible(const Type *from, const Type *to);
  static const Type *commonNumericType(const Type *a, const Type *b);
  // The type both `a` and `b` convert to (for `?:`, `??` and array
  // literals), or null.
  static const Type *unify(const Type *a, const Type *b);
  // The type in which `===` compares values of types `a` and `b` (neither
  // undefined), or null if they cannot be compared.
  static const Type *comparisonType(const Type *a, const Type *b);

private:
  Sema &sema_;
};

} // namespace tz
