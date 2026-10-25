#pragma once

#include "compiler/lower/codegen/codegen.h"

namespace tz {

// decimal.
class DecimalLowerer {
public:
  explicit DecimalLowerer(CodeGen &codegen);

  // Decimals (codegen_decimal.cpp). The runtime takes a decimal as its two
  // words and returns one through a pointer passed first, as for strings.
  llvm::FunctionCallee decimalFunction(llvm::StringRef name, llvm::Type *ret,
                                       llvm::ArrayRef<llvm::Type *> params);
  llvm::Value *decimalCoef(llvm::Value *value);
  llvm::Value *decimalMeta(llvm::Value *value);
  llvm::Value *callReturningDecimal(llvm::StringRef name, llvm::ArrayRef<llvm::Value *> args);
  // A literal (or its negation) of type decimal: a constant.
  llvm::Constant *emitDecimalLiteral(const Expr *lit);
  llvm::Value *decimalFromInteger(llvm::Value *value);
  // `a op b` of borrowed decimals (for `**`, b is a long); owned result.
  llvm::Value *emitDecimalArithmetic(BinaryOp op, llvm::Value *lhs, llvm::Value *rhs);
  // Arithmetic and comparisons of decimals.
  llvm::Value *emitDecimalBinary(BinaryExpr *bin);
  // `x op= v` of a decimal whose old value `old` is borrowed from its slot.
  llvm::Value *emitDecimalCompound(BinaryOp op, llvm::Value *old, Expr *value);
  llvm::Value *emitDecimalUpdate(UpdateExpr *update, llvm::Value *address);
  llvm::Value *emitDecimalCall(CallExpr *call);
  llvm::Value *emitDecimalFold(CallExpr *call);
  // The sign of a borrowed decimal as an i32: -1, 0 or 1.
  llvm::Value *decimalSign(llvm::Value *value);
  // An i32 rounding mode from a string argument, or half-up without one.
  llvm::Value *emitRoundingMode(CallExpr *call, size_t index);
  // tz.dec.retain and tz.dec.release, made when first needed.
  llvm::Function *decimalRefCount(bool retain);

private:
  CodeGen &codegen_;
  llvm::Function *decRetainFn_ = nullptr;
  llvm::Function *decReleaseFn_ = nullptr;
};

} // namespace tz
