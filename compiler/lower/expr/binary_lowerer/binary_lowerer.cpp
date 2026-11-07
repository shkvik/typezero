#include "compiler/lower/expr/binary_lowerer/binary_lowerer.h"
#include "compiler/library/decimal/decimal_lowerer/decimal_lowerer.h"
#include "compiler/library/process/process_lowerer/process_lowerer.h"
#include "compiler/lower/classes/class_lowerer/class_lowerer.h"
#include "compiler/lower/expr/equality_lowerer/equality_lowerer.h"
#include "compiler/lower/expr/expr_lowerer/expr_lowerer.h"
#include "compiler/lower/expr/truth_lowerer/truth_lowerer.h"
#include "compiler/lower/function_frame/function_frame.h"
#include "compiler/lower/module_builder/module_builder.h"
#include "compiler/lower/types/union_member_lowerer/union_member_lowerer.h"
#include "compiler/lower/values/number_lowerer/number_lowerer.h"
#include "compiler/lower/values/string_lowerer/string_lowerer.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/IR/Intrinsics.h"
#include "llvm/IR/MDBuilder.h"
#include "llvm/Support/ErrorHandling.h"
#include <optional>

using llvm::dyn_cast;

namespace tz {

BinaryLowerer::BinaryLowerer(Lowering &owner)
    : lowering(owner) {}

llvm::Value *BinaryLowerer::emitBinary(BinaryExpr *bin) {
  switch (bin->op) {
  case BinaryOp::LogAnd:
  case BinaryOp::LogOr:
    return this->lowering.truthLowerer().emitLogical(bin);
  case BinaryOp::Nullish:
    return this->lowering.equalityLowerer().emitNullish(bin);
  case BinaryOp::Eq:
  case BinaryOp::Ne:
    return this->lowering.equalityLowerer().emitEquality(bin);
  case BinaryOp::InstanceOf:
    return this->lowering.classLowerer().emitInstanceOf(bin);
  case BinaryOp::In:
    return this->lowering.unionMemberLowerer().emitObjectIn(bin);
  default:
    break;
  }
  if (bin->type->isString()) {
    return this->lowering.stringLowerer().emitConcat(bin);
  }
  if (bin->lhs->type->isString()) {
    return this->lowering.stringLowerer().emitStringComparison(bin);
  }
  if (bin->lhs->type->isDecimal()) {
    return this->lowering.decimalLowerer().emitDecimalBinary(bin);
  }

  llvm::Value *lhs = this->lowering.exprLowerer().emitExpr(bin->lhs);
  llvm::Value *rhs = this->lowering.exprLowerer().emitExpr(bin->rhs);
  switch (bin->op) {
  case BinaryOp::Lt:
  case BinaryOp::Le:
  case BinaryOp::Gt:
  case BinaryOp::Ge:
    if (bin->lhs->type != bin->rhs->type && bin->lhs->type->isInteger() &&
        bin->rhs->type->isInteger()) {
      return this->emitMixedComparison(bin->op, lhs, rhs, bin->lhs->type, bin->rhs->type);
    }
    return this->emitComparison(bin->op, lhs, rhs, bin->lhs->type);
  default:
    return this->emitArithmetic(bin->op, lhs, rhs, bin->type, bin->rangeProven);
  }
}

llvm::Value *BinaryLowerer::emitArithmetic(BinaryOp op, llvm::Value *lhs, llvm::Value *rhs,
                                           const Type *type, bool proven) {
  bool isFloat = type->isFloat();
  bool bitwise = op == BinaryOp::BitAnd || op == BinaryOp::BitOr || op == BinaryOp::BitXor ||
                 op == BinaryOp::Shl || op == BinaryOp::Shr || op == BinaryOp::UShr;
  llvm::IRBuilder<> &b = this->lowering.builder_;
  std::optional<llvm::IRBuilder<>::FastMathFlagGuard> finite;
  if (isFloat && proven) {
    finite.emplace(b);
    llvm::FastMathFlags flags;
    flags.setNoNaNs();
    flags.setNoInfs();
    b.setFastMathFlags(flags);
  }
  if (bitwise && type->isWrapping() &&
      (op == BinaryOp::Shl || op == BinaryOp::Shr || op == BinaryOp::UShr)) {
    llvm::Value *width = llvm::ConstantInt::get(lhs->getType(), type->bitWidth());
    llvm::Value *count =
        b.CreateAnd(rhs, llvm::ConstantInt::get(lhs->getType(), type->bitWidth() - 1));
    llvm::Value *shifted = op == BinaryOp::Shl ? b.CreateShl(lhs, count) : b.CreateLShr(lhs, count);
    return b.CreateSelect(b.CreateICmpUGE(rhs, width), llvm::ConstantInt::get(lhs->getType(), 0),
                          shifted);
  }
  switch (op) {
  case BinaryOp::Add:
    return isFloat ? this->lowering.builder_.CreateFAdd(lhs, rhs)
                   : this->lowering.builder_.CreateAdd(lhs, rhs);
  case BinaryOp::Sub:
    return isFloat ? this->lowering.builder_.CreateFSub(lhs, rhs)
                   : this->lowering.builder_.CreateSub(lhs, rhs);
  case BinaryOp::Mul:
    return isFloat ? this->lowering.builder_.CreateFMul(lhs, rhs)
                   : this->lowering.builder_.CreateMul(lhs, rhs);
  case BinaryOp::Div:
    return isFloat ? this->lowering.builder_.CreateFDiv(lhs, rhs)
                   : this->emitIntDivRem(op, lhs, rhs, type, proven);
  case BinaryOp::Rem:
    return isFloat ? this->lowering.builder_.CreateFRem(lhs, rhs)
                   : this->emitIntDivRem(op, lhs, rhs, type, proven);
  case BinaryOp::Pow: {
    if (isFloat) {
      return this->lowering.builder_.CreateCall(this->lowering.rt_.mathPow, {lhs, rhs});
    }
    llvm::Value *result = this->lowering.builder_.CreateCall(
        this->lowering.rt_.mathPowLong, {this->lowering.numberLowerer().toI64(lhs, type),
                                         this->lowering.numberLowerer().toI64(rhs, type)});
    return this->lowering.builder_.CreateTrunc(result, lhs->getType());
  }
  case BinaryOp::BitAnd:
    return this->lowering.builder_.CreateAnd(lhs, rhs);
  case BinaryOp::BitOr:
    return this->lowering.builder_.CreateOr(lhs, rhs);
  case BinaryOp::BitXor:
    return this->lowering.builder_.CreateXor(lhs, rhs);
  case BinaryOp::Shl:
  case BinaryOp::Shr:
  case BinaryOp::UShr: {
    llvm::Value *mask = llvm::ConstantInt::get(lhs->getType(), type->bitWidth() - 1);
    llvm::Value *count = this->lowering.builder_.CreateAnd(rhs, mask);
    if (op == BinaryOp::Shl) {
      return this->lowering.builder_.CreateShl(lhs, count);
    }
    if (op == BinaryOp::Shr && !type->isUnsigned()) {
      return this->lowering.builder_.CreateAShr(lhs, count);
    }
    return this->lowering.builder_.CreateLShr(lhs, count);
  }
  default:
    break;
  }
  llvm_unreachable("not an arithmetic operator");
}

llvm::Value *BinaryLowerer::emitComparison(BinaryOp op, llvm::Value *lhs, llvm::Value *rhs,
                                           const Type *type) {
  if (type->isFloat()) {
    switch (op) {
    case BinaryOp::Lt:
      return this->lowering.builder_.CreateFCmpOLT(lhs, rhs);
    case BinaryOp::Le:
      return this->lowering.builder_.CreateFCmpOLE(lhs, rhs);
    case BinaryOp::Gt:
      return this->lowering.builder_.CreateFCmpOGT(lhs, rhs);
    case BinaryOp::Ge:
      return this->lowering.builder_.CreateFCmpOGE(lhs, rhs);
    case BinaryOp::Eq:
      return this->lowering.builder_.CreateFCmpOEQ(lhs, rhs);
    case BinaryOp::Ne:
      return this->lowering.builder_.CreateFCmpUNE(lhs, rhs);
    default:
      break;
    }
  } else {
    bool u = type->isInteger() && type->isUnsigned();
    switch (op) {
    case BinaryOp::Lt:
      return u ? this->lowering.builder_.CreateICmpULT(lhs, rhs)
               : this->lowering.builder_.CreateICmpSLT(lhs, rhs);
    case BinaryOp::Le:
      return u ? this->lowering.builder_.CreateICmpULE(lhs, rhs)
               : this->lowering.builder_.CreateICmpSLE(lhs, rhs);
    case BinaryOp::Gt:
      return u ? this->lowering.builder_.CreateICmpUGT(lhs, rhs)
               : this->lowering.builder_.CreateICmpSGT(lhs, rhs);
    case BinaryOp::Ge:
      return u ? this->lowering.builder_.CreateICmpUGE(lhs, rhs)
               : this->lowering.builder_.CreateICmpSGE(lhs, rhs);
    case BinaryOp::Eq:
      return this->lowering.builder_.CreateICmpEQ(lhs, rhs);
    case BinaryOp::Ne:
      return this->lowering.builder_.CreateICmpNE(lhs, rhs);
    default:
      break;
    }
  }
  llvm_unreachable("not a comparison operator");
}

llvm::Value *BinaryLowerer::emitMixedComparison(BinaryOp op, llvm::Value *lhs, llvm::Value *rhs,
                                                const Type *l, const Type *r) {
  llvm::IRBuilder<> &b = this->lowering.builder_;
  llvm::Type *wide = b.getIntNTy(65);
  lhs = l->isUnsigned() ? b.CreateZExt(lhs, wide) : b.CreateSExt(lhs, wide);
  rhs = r->isUnsigned() ? b.CreateZExt(rhs, wide) : b.CreateSExt(rhs, wide);

  return this->emitComparison(op, lhs, rhs, Type::intTy());
}

llvm::Value *BinaryLowerer::emitIntDivRem(BinaryOp op, llvm::Value *lhs, llvm::Value *rhs,
                                          const Type *type, bool proven) {
  bool isRem = op == BinaryOp::Rem;
  bool u = type->isUnsigned();
  auto *known = dyn_cast<llvm::ConstantInt>(rhs);
  if (proven || (known && !known->isZero() && (u || !known->isMinusOne()))) {
    return u       ? (isRem ? this->lowering.builder_.CreateURem(lhs, rhs)
                            : this->lowering.builder_.CreateUDiv(lhs, rhs))
           : isRem ? this->lowering.builder_.CreateSRem(lhs, rhs)
                   : this->lowering.builder_.CreateSDiv(lhs, rhs);
  }

  llvm::Type *width = lhs->getType();
  llvm::Value *isZero = this->lowering.builder_.CreateICmpEQ(rhs, llvm::ConstantInt::get(width, 0));
  llvm::BasicBlock *throwBlock = this->lowering.functionFrame().newBlock("div.zero");
  llvm::BasicBlock *okBlock = this->lowering.functionFrame().newBlock("div.ok");
  this->lowering.builder_.CreateCondBr(
      isZero, throwBlock, okBlock,
      llvm::MDBuilder(this->lowering.ctx_).createUnlikelyBranchWeights());
  this->lowering.functionFrame().switchTo(throwBlock);
  this->lowering.processLowerer().emitPanic("Division by zero");

  this->lowering.functionFrame().switchTo(okBlock);
  if (u) {
    return isRem ? this->lowering.builder_.CreateURem(lhs, rhs)
                 : this->lowering.builder_.CreateUDiv(lhs, rhs);
  }
  llvm::Value *isMinusOne =
      this->lowering.builder_.CreateICmpEQ(rhs, llvm::ConstantInt::getSigned(width, -1));
  llvm::Value *divisor =
      this->lowering.builder_.CreateSelect(isMinusOne, llvm::ConstantInt::get(width, 1), rhs);
  if (isRem) {
    return this->lowering.builder_.CreateSRem(lhs, divisor);
  }
  llvm::Value *quotient = this->lowering.builder_.CreateSDiv(lhs, divisor);

  return this->lowering.builder_.CreateSelect(isMinusOne, this->lowering.builder_.CreateNeg(lhs),
                                              quotient);
}

}
