#include "compiler/lower/expr/binary_lowerer/binary_lowerer.h"
#include "compiler/library/decimal/decimal_lowerer/decimal_lowerer.h"
#include "compiler/lower/classes/class_lowerer/class_lowerer.h"
#include "compiler/lower/errors/throw_lowerer/throw_lowerer.h"
#include "compiler/lower/expr/equality_lowerer/equality_lowerer.h"
#include "compiler/lower/expr/expr_lowerer/expr_lowerer.h"
#include "compiler/lower/expr/truth_lowerer/truth_lowerer.h"
#include "compiler/lower/function_frame/function_frame.h"
#include "compiler/lower/module_builder/module_builder.h"
#include "compiler/lower/types/union_member_lowerer/union_member_lowerer.h"
#include "compiler/lower/values/string_lowerer/string_lowerer.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/IR/Intrinsics.h"
#include "llvm/IR/MDBuilder.h"
#include "llvm/Support/ConvertUTF.h"
#include "llvm/Support/ErrorHandling.h"
#include <array>

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
  case BinaryOp::In: // of a union, which Checker allows
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
    return emitComparison(bin->op, lhs, rhs, bin->lhs->type);
  case BinaryOp::UShr:
    // An int's `x >>> n` that Checker made a long: the unsigned 32-bit result.
    if (bin->type != bin->lhs->type) {
      return lowering_.builder_.CreateZExt(emitArithmetic(bin->op, lhs, rhs, bin->lhs->type),
                                           lowering_.moduleBuilder().lower(bin->type));
    }
    return emitArithmetic(bin->op, lhs, rhs, bin->type);
  default:
    return emitArithmetic(bin->op, lhs, rhs, bin->type);
  }
}

llvm::Value *BinaryLowerer::emitArithmetic(BinaryOp op, llvm::Value *lhs, llvm::Value *rhs,
                                           const Type *type) {
  bool isFloat = type->isFloat();
  bool bitwise = op == BinaryOp::BitAnd || op == BinaryOp::BitOr || op == BinaryOp::BitXor ||
                 op == BinaryOp::Shl || op == BinaryOp::Shr || op == BinaryOp::UShr;
  if (isFloat && bitwise) {
    // `x |= v` where x is a double: on 32-bit integers, as in JavaScript (Checker
    // made v an int), and the result goes back as a double; that of `>>>=`
    // is unsigned.
    llvm::Value *result = emitArithmetic(
        op, lowering_.builder_.CreateCall(lowering_.rt_.toInt32, {lhs}), rhs, Type::intTy());
    return op == BinaryOp::UShr ? lowering_.builder_.CreateUIToFP(result, lhs->getType())
                                : lowering_.builder_.CreateSIToFP(result, lhs->getType());
  }
  if (bitwise && lhs->getType() != rhs->getType()) {
    // `x |= v` where x is a long and v an int: on 32-bit integers too (Checker
    // left v an int), and the result goes back as a long.
    llvm::Value *result =
        emitArithmetic(op, lowering_.builder_.CreateTrunc(lhs, rhs->getType()), rhs, Type::intTy());
    return lowering_.builder_.CreateSExt(result, lhs->getType());
  }
  switch (op) {
  // Integer arithmetic wraps around on overflow, as in Go.
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
    return isFloat ? lowering_.builder_.CreateFDiv(lhs, rhs) : emitIntDivRem(op, lhs, rhs);
  case BinaryOp::Rem:
    return isFloat ? lowering_.builder_.CreateFRem(lhs, rhs) : emitIntDivRem(op, lhs, rhs);
  case BinaryOp::Pow: {
    if (isFloat) {
      return lowering_.builder_.CreateCall(lowering_.rt_.mathPow, {lhs, rhs});
    }
    // Integers: computed as longs, which wrap around like repeated `*`; the
    // low half of that is the int result.
    llvm::Type *i64 = lowering_.builder_.getInt64Ty();
    llvm::Value *result = lowering_.builder_.CreateCall(
        lowering_.rt_.mathPowLong,
        {lowering_.builder_.CreateSExt(lhs, i64), lowering_.builder_.CreateSExt(rhs, i64)});
    lowering_.throwLowerer().emitThrowCheck(); // 0 ** -1
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
    // As in JavaScript, only the low bits of the count are used. This also
    // keeps the shift defined for any count.
    llvm::Value *mask = llvm::ConstantInt::get(lhs->getType(), type->bitWidth() - 1);
    llvm::Value *count = lowering_.builder_.CreateAnd(rhs, mask);
    if (op == BinaryOp::Shl) {
      return lowering_.builder_.CreateShl(lhs, count);
    }
    if (op == BinaryOp::Shr) {
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
      return lowering_.builder_.CreateFCmpUNE(lhs, rhs); // NaN != NaN
    default:
      break;
    }
  } else {
    switch (op) {
    case BinaryOp::Lt:
      return lowering_.builder_.CreateICmpSLT(lhs, rhs);
    case BinaryOp::Le:
      return lowering_.builder_.CreateICmpSLE(lhs, rhs);
    case BinaryOp::Gt:
      return lowering_.builder_.CreateICmpSGT(lhs, rhs);
    case BinaryOp::Ge:
      return lowering_.builder_.CreateICmpSGE(lhs, rhs);
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

// Dividing by zero throws RangeError, as JavaScript's `1n / 0n`. MIN / -1
// wraps around to MIN (and MIN % -1 is 0) where the machine instruction
// would trap.
llvm::Value *BinaryLowerer::emitIntDivRem(BinaryOp op, llvm::Value *lhs, llvm::Value *rhs) {
  bool isRem = op == BinaryOp::Rem;
  if (auto *divisor = dyn_cast<llvm::ConstantInt>(rhs);
      divisor && !divisor->isZero() && !divisor->isMinusOne()) {
    return isRem ? lowering_.builder_.CreateSRem(lhs, rhs)
                 : lowering_.builder_.CreateSDiv(lhs, rhs);
  }

  llvm::Type *type = lhs->getType();
  llvm::Value *isZero = lowering_.builder_.CreateICmpEQ(rhs, llvm::ConstantInt::get(type, 0));
  llvm::BasicBlock *throwBlock = lowering_.functionFrame().newBlock("div.zero");
  llvm::BasicBlock *okBlock = lowering_.functionFrame().newBlock("div.ok");
  lowering_.builder_.CreateCondBr(isZero, throwBlock, okBlock,
                                  llvm::MDBuilder(lowering_.ctx_).createUnlikelyBranchWeights());
  lowering_.functionFrame().switchTo(throwBlock);
  lowering_.throwLowerer().emitThrowNew(2, "Division by zero");

  lowering_.functionFrame().switchTo(okBlock);
  llvm::Value *isMinusOne =
      lowering_.builder_.CreateICmpEQ(rhs, llvm::ConstantInt::getSigned(type, -1));
  llvm::Value *divisor =
      lowering_.builder_.CreateSelect(isMinusOne, llvm::ConstantInt::get(type, 1), rhs);
  if (isRem) {
    return lowering_.builder_.CreateSRem(lhs, divisor); // x % 1 == x % -1 == 0
  }
  llvm::Value *quotient = lowering_.builder_.CreateSDiv(lhs, divisor);

  return lowering_.builder_.CreateSelect(isMinusOne, lowering_.builder_.CreateNeg(lhs), quotient);
}

} // namespace tz
