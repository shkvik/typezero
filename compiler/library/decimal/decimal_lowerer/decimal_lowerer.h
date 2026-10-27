#pragma once

#include "compiler/lower/lowering/lowering.h"

namespace tz {

class DecimalLowerer {
public:
  explicit DecimalLowerer(Lowering &lowering);

  llvm::FunctionCallee decimalFunction(llvm::StringRef name, llvm::Type *ret,
                                       llvm::ArrayRef<llvm::Type *> params);
  llvm::Value *decimalCoef(llvm::Value *value);
  llvm::Value *decimalMeta(llvm::Value *value);
  llvm::Value *callReturningDecimal(llvm::StringRef name, llvm::ArrayRef<llvm::Value *> args);
  llvm::Constant *emitDecimalLiteral(const Expr *lit);
  llvm::Value *decimalFromInteger(llvm::Value *value, const Type *from);
  llvm::Value *emitDecimalArithmetic(BinaryOp op, llvm::Value *lhs, llvm::Value *rhs);
  llvm::Value *emitDecimalBinary(BinaryExpr *bin);
  llvm::Value *emitDecimalCompound(BinaryOp op, llvm::Value *old, Expr *value);
  llvm::Value *emitDecimalUpdate(UpdateExpr *update, llvm::Value *address);
  llvm::Value *emitDecimalCall(CallExpr *call);
  llvm::Value *emitDecimalFold(CallExpr *call);
  llvm::Value *decimalSign(llvm::Value *value);
  llvm::Value *emitRoundingMode(CallExpr *call, size_t index);
  llvm::Function *decimalRefCount(bool retain);

private:
  Lowering &lowering_;
  llvm::Function *decRetainFn_ = nullptr;
  llvm::Function *decReleaseFn_ = nullptr;
};

}
