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
#include "llvm/Support/ConvertUTF.h"
#include "llvm/Support/ErrorHandling.h"
#include <array>
#include <optional>

using llvm::BasicBlock;
using llvm::cast;
using llvm::dyn_cast;
using llvm::isa;
using llvm::Value;

namespace tz {

BinaryLowerer::BinaryLowerer(Lowering &lowering) : lowering_(lowering) {}

llvm::Value *BinaryLowerer::emitBinary(BinaryExpr *bin) {
  switch (bin->op) {
  case BinaryOp::LogAnd:
  case BinaryOp::LogOr:
    return lowering_.truthLowerer().emitLogical(bin);
  case BinaryOp::Nullish:
    return lowering_.equalityLowerer().emitNullish(bin);
  case BinaryOp::Eq:
  case BinaryOp::Ne:
    return lowering_.equalityLowerer().emitEquality(bin);
  case BinaryOp::InstanceOf:
    return lowering_.classLowerer().emitInstanceOf(bin);
  case BinaryOp::In:
    return lowering_.unionMemberLowerer().emitObjectIn(bin);
  default:
    break;
  }
  if (bin->type->isString()) {
    return lowering_.stringLowerer().emitConcat(bin);
  }
  if (bin->lhs->type->isString()) {
    return lowering_.stringLowerer().emitStringComparison(bin);
  }
  if (bin->lhs->type->isDecimal()) {
    return lowering_.decimalLowerer().emitDecimalBinary(bin);
  }

  llvm::Value *lhs = lowering_.exprLowerer().emitExpr(bin->lhs);
  llvm::Value *rhs = lowering_.exprLowerer().emitExpr(bin->rhs);
  switch (bin->op) {
  case BinaryOp::Lt:
  case BinaryOp::Le:
  case BinaryOp::Gt:
  case BinaryOp::Ge:
    if (bin->lhs->type != bin->rhs->type && bin->lhs->type->isInteger() &&
        bin->rhs->type->isInteger()) {
      return emitMixedComparison(bin->op, lhs, rhs, bin->lhs->type, bin->rhs->type);
    }
    return emitComparison(bin->op, lhs, rhs, bin->lhs->type);
  default:
    return emitArithmetic(bin->op, lhs, rhs, bin->type, bin->rangeProven);
  }
}

llvm::Value *BinaryLowerer::emitArithmetic(BinaryOp op, llvm::Value *lhs, llvm::Value *rhs,
                                           const Type *type, bool proven) {
  bool isFloat = type->isFloat();
  bool bitwise = op == BinaryOp::BitAnd || op == BinaryOp::BitOr || op == BinaryOp::BitXor ||
                 op == BinaryOp::Shl || op == BinaryOp::Shr || op == BinaryOp::UShr;
  llvm::IRBuilder<> &b = lowering_.builder_;
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
    return isFloat ? lowering_.builder_.CreateFAdd(lhs, rhs)
                   : lowering_.builder_.CreateAdd(lhs, rhs);
  case BinaryOp::Sub:
    return isFloat ? lowering_.builder_.CreateFSub(lhs, rhs)
                   : lowering_.builder_.CreateSub(lhs, rhs);
  case BinaryOp::Mul:
    return isFloat ? lowering_.builder_.CreateFMul(lhs, rhs)
                   : lowering_.builder_.CreateMul(lhs, rhs);
  case BinaryOp::Div:
    return isFloat ? lowering_.builder_.CreateFDiv(lhs, rhs)
                   : emitIntDivRem(op, lhs, rhs, type, proven);
  case BinaryOp::Rem:
    return isFloat ? lowering_.builder_.CreateFRem(lhs, rhs)
                   : emitIntDivRem(op, lhs, rhs, type, proven);
  case BinaryOp::Pow: {
    if (isFloat) {
      return lowering_.builder_.CreateCall(lowering_.rt_.mathPow, {lhs, rhs});
    }
    llvm::Value *result = lowering_.builder_.CreateCall(
        lowering_.rt_.mathPowLong,
        {lowering_.numberLowerer().toI64(lhs, type), lowering_.numberLowerer().toI64(rhs, type)});
    return lowering_.builder_.CreateTrunc(result, lhs->getType());
  }
  case BinaryOp::BitAnd:
    return lowering_.builder_.CreateAnd(lhs, rhs);
  case BinaryOp::BitOr:
    return lowering_.builder_.CreateOr(lhs, rhs);
  case BinaryOp::BitXor:
    return lowering_.builder_.CreateXor(lhs, rhs);
  case BinaryOp::Shl:
  case BinaryOp::Shr:
  case BinaryOp::UShr: {
    llvm::Value *mask = llvm::ConstantInt::get(lhs->getType(), type->bitWidth() - 1);
    llvm::Value *count = lowering_.builder_.CreateAnd(rhs, mask);
    if (op == BinaryOp::Shl) {
      return lowering_.builder_.CreateShl(lhs, count);
    }
    if (op == BinaryOp::Shr && !type->isUnsigned()) {
      return lowering_.builder_.CreateAShr(lhs, count);
    }
    return lowering_.builder_.CreateLShr(lhs, count);
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
      return lowering_.builder_.CreateFCmpOLT(lhs, rhs);
    case BinaryOp::Le:
      return lowering_.builder_.CreateFCmpOLE(lhs, rhs);
    case BinaryOp::Gt:
      return lowering_.builder_.CreateFCmpOGT(lhs, rhs);
    case BinaryOp::Ge:
      return lowering_.builder_.CreateFCmpOGE(lhs, rhs);
    case BinaryOp::Eq:
      return lowering_.builder_.CreateFCmpOEQ(lhs, rhs);
    case BinaryOp::Ne:
      return lowering_.builder_.CreateFCmpUNE(lhs, rhs);
    default:
      break;
    }
  } else {
    bool u = type->isInteger() && type->isUnsigned();
    switch (op) {
    case BinaryOp::Lt:
      return u ? lowering_.builder_.CreateICmpULT(lhs, rhs)
               : lowering_.builder_.CreateICmpSLT(lhs, rhs);
    case BinaryOp::Le:
      return u ? lowering_.builder_.CreateICmpULE(lhs, rhs)
               : lowering_.builder_.CreateICmpSLE(lhs, rhs);
    case BinaryOp::Gt:
      return u ? lowering_.builder_.CreateICmpUGT(lhs, rhs)
               : lowering_.builder_.CreateICmpSGT(lhs, rhs);
    case BinaryOp::Ge:
      return u ? lowering_.builder_.CreateICmpUGE(lhs, rhs)
               : lowering_.builder_.CreateICmpSGE(lhs, rhs);
    case BinaryOp::Eq:
      return lowering_.builder_.CreateICmpEQ(lhs, rhs);
    case BinaryOp::Ne:
      return lowering_.builder_.CreateICmpNE(lhs, rhs);
    default:
      break;
    }
  }
  llvm_unreachable("not a comparison operator");
}

llvm::Value *BinaryLowerer::emitMixedComparison(BinaryOp op, llvm::Value *lhs, llvm::Value *rhs,
                                                const Type *l, const Type *r) {
  llvm::IRBuilder<> &b = lowering_.builder_;
  llvm::Type *wide = b.getIntNTy(65);
  lhs = l->isUnsigned() ? b.CreateZExt(lhs, wide) : b.CreateSExt(lhs, wide);
  rhs = r->isUnsigned() ? b.CreateZExt(rhs, wide) : b.CreateSExt(rhs, wide);

  return emitComparison(op, lhs, rhs, Type::intTy());
}

llvm::Value *BinaryLowerer::emitIntDivRem(BinaryOp op, llvm::Value *lhs, llvm::Value *rhs,
                                          const Type *type, bool proven) {
  bool isRem = op == BinaryOp::Rem;
  bool u = type->isUnsigned();
  auto *known = dyn_cast<llvm::ConstantInt>(rhs);
  if (proven || (known && !known->isZero() && (u || !known->isMinusOne()))) {
    return u       ? (isRem ? lowering_.builder_.CreateURem(lhs, rhs)
                            : lowering_.builder_.CreateUDiv(lhs, rhs))
           : isRem ? lowering_.builder_.CreateSRem(lhs, rhs)
                   : lowering_.builder_.CreateSDiv(lhs, rhs);
  }

  llvm::Type *width = lhs->getType();
  llvm::Value *isZero = lowering_.builder_.CreateICmpEQ(rhs, llvm::ConstantInt::get(width, 0));
  llvm::BasicBlock *throwBlock = lowering_.functionFrame().newBlock("div.zero");
  llvm::BasicBlock *okBlock = lowering_.functionFrame().newBlock("div.ok");
  lowering_.builder_.CreateCondBr(isZero, throwBlock, okBlock,
                                  llvm::MDBuilder(lowering_.ctx_).createUnlikelyBranchWeights());
  lowering_.functionFrame().switchTo(throwBlock);
  lowering_.processLowerer().emitPanic("Division by zero");

  lowering_.functionFrame().switchTo(okBlock);
  if (u) {
    return isRem ? lowering_.builder_.CreateURem(lhs, rhs)
                 : lowering_.builder_.CreateUDiv(lhs, rhs);
  }
  llvm::Value *isMinusOne =
      lowering_.builder_.CreateICmpEQ(rhs, llvm::ConstantInt::getSigned(width, -1));
  llvm::Value *divisor =
      lowering_.builder_.CreateSelect(isMinusOne, llvm::ConstantInt::get(width, 1), rhs);
  if (isRem) {
    return lowering_.builder_.CreateSRem(lhs, divisor);
  }
  llvm::Value *quotient = lowering_.builder_.CreateSDiv(lhs, divisor);

  return lowering_.builder_.CreateSelect(isMinusOne, lowering_.builder_.CreateNeg(lhs), quotient);
}

}
