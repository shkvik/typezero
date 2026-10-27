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
  static bool isIntLiteralTree(const Expr *expr);
  bool literalAdapts(const Expr *expr, const Type *to);
  const Type *literalTarget(const Expr *expr, const Type *to);
  Expr *adaptLiteral(Expr *expr, const Type *to);
  static std::string operandText(llvm::StringRef text);
  static std::string conversionHint(const Expr *expr, const Type *to);
  static std::string conversionMethod(const Type *to);

private:
  Checker &checker_;
};

}
