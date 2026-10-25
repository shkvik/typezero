#pragma once

#include "compiler/check/checker/checker.h"

namespace tz {

// The values of constant expressions: enum members and the constants of global objects.
class ConstantFolder {
public:
  explicit ConstantFolder(Checker &checker);

  std::optional<Checker::Constant> evalConstant(const Expr *expr, EnumDecl *self, size_t defined);
  // Integer constants (sema_decls.cpp): expressions of literals, `const`
  // variables holding constants, and operators. Computing them reports what
  // would go wrong at run time: a division by zero, or a value that does not
  // fit in its type and would wrap around where JavaScript keeps it.
  std::optional<int64_t> intConstant(const Expr *expr) const;
  void foldUnary(UnaryExpr *unary, bool wraps);
  void foldBinary(BinaryExpr *bin, bool wraps);
  // `x / 0` and `x % 0` of integers: an error even when x is not constant.
  void checkDivisor(BinaryOp op, const Expr *divisor);
  void reportOverflow(const Expr *expr, const Type *type, const std::string &value, int64_t wrapped,
                      bool wraps);
  // Math.PI, Number.MAX_SAFE_INTEGER, NaN...: null if `name` is not one.
  Expr *globalConstant(llvm::StringRef object, llvm::StringRef name, llvm::SMLoc loc);

private:
  Checker &checker_;
  // The values of the integer constants computed so far: operator expressions
  // and `const` variables. In a `wraps` context a value may exceed its type
  // (see foldBinary).
  llvm::DenseMap<const Expr *, int64_t> constants_;
};

} // namespace tz
