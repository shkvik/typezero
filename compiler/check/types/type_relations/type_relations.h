#pragma once

#include "compiler/check/checker/checker.h"

namespace tz {

class TypeRelations {
public:
  explicit TypeRelations(Checker &checker);

  Expr *convert(Expr *expr, const Type *to);
  Expr *coerce(Expr *expr, const Type *to, llvm::StringRef what = {});
  static bool isImplicitlyConvertible(const Type *from, const Type *to);
  static const Type *commonNumericType(const Type *a, const Type *b);
  static const Type *unify(const Type *a, const Type *b);
  static const Type *comparisonType(const Type *a, const Type *b);

private:
  Checker &checker_;
};

}
