#pragma once

#include "compiler/check/checker/checker.h"

namespace tz {

class ConstantFolder {
public:
  explicit ConstantFolder(Checker &checker);

  std::optional<Checker::Constant> evalConstant(const Expr *expr, EnumDecl *self, size_t defined);
  std::optional<int64_t> intConstant(const Expr *expr) const;
  std::optional<uint64_t> unsignedConstant(const Expr *expr) const;
  void noteUnsignedVar(const VarSymbol *var, const Expr *init);
  void foldUnary(UnaryExpr *unary, bool wraps);
  void foldBinary(BinaryExpr *bin, bool wraps);
  void checkDivisor(BinaryOp op, const Expr *divisor);
  void reportOverflow(const Expr *expr, const Type *type, const std::string &value, int64_t wrapped,
                      bool wraps);
  Expr *globalConstant(llvm::StringRef object, llvm::StringRef name, llvm::SMLoc loc);

private:
  Checker &checker_;
  llvm::DenseMap<const Expr *, int64_t> constants_;
  llvm::DenseMap<const Expr *, uint64_t> bigUnsigned_;
  llvm::DenseMap<const VarSymbol *, uint64_t> bigUnsignedVars_;
};

}
