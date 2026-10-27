#include "compiler/library/math/math_lowerer/math_lowerer.h"
#include "compiler/lower/expr/expr_lowerer/expr_lowerer.h"
#include "compiler/lower/function_frame/function_frame.h"
#include "compiler/lower/layouts/object_layouts/object_layouts.h"
#include "compiler/lower/memory/ref_counts/ref_counts.h"
#include "compiler/lower/module_builder/module_builder.h"
#include "compiler/lower/types/optional_lowerer/optional_lowerer.h"
#include "compiler/lower/values/array_lowerer/array_lowerer.h"
#include "compiler/lower/values/number_lowerer/number_lowerer.h"
#include "llvm/IR/Intrinsics.h"
#include "llvm/IR/MDBuilder.h"
#include "llvm/Support/ErrorHandling.h"

using llvm::BasicBlock;
using llvm::cast;
using llvm::dyn_cast;
using llvm::isa;
using llvm::Value;

namespace tz {

MathLowerer::MathLowerer(Lowering &lowering) : lowering_(lowering) {}

llvm::FunctionCallee MathLowerer::mathFunction(llvm::StringRef name, unsigned params) {
  llvm::Type *f64 = lowering_.builder_.getDoubleTy();
  std::vector<llvm::Type *> types(params, f64);
  llvm::FunctionCallee callee = lowering_.module_->getOrInsertFunction(
      ("tz_math_" + name).str(), llvm::FunctionType::get(f64, types, false));
  cast<llvm::Function>(callee.getCallee())->addFnAttr(llvm::Attribute::NoUnwind);

  return callee;
}

llvm::Value *MathLowerer::spillArray(llvm::ArrayRef<llvm::Value *> values, llvm::Type *element) {
  auto *arrayTy = llvm::ArrayType::get(element, values.size());
  llvm::AllocaInst *array = lowering_.functionFrame().createStackSlot(arrayTy, "args");
  for (size_t i = 0; i < values.size(); ++i) {
    lowering_.builder_.CreateStore(values[i], lowering_.builder_.CreateConstInBoundsGEP2_32(
                                                  arrayTy, array, 0, static_cast<unsigned>(i)));
  }

  return array;
}

llvm::Value *MathLowerer::emitMathCall(CallExpr *call) {
  std::vector<llvm::Value *> args;
  for (Expr *arg : call->args) {
    args.push_back(lowering_.exprLowerer().emitExpr(arg));
  }
  const Type *type = call->type;
  llvm::Type *f64 = lowering_.builder_.getDoubleTy();
  llvm::Value *x = args.empty() ? nullptr : args[0];

  if (isRoundingBuiltin(call->builtin)) {
    const Type *from = call->args[0]->type;
    if (from->isFloat()) {
      switch (call->builtin) {
      case Builtin::MathFloor:
        x = lowering_.builder_.CreateUnaryIntrinsic(llvm::Intrinsic::floor, x);
        break;
      case Builtin::MathCeil:
        x = lowering_.builder_.CreateUnaryIntrinsic(llvm::Intrinsic::ceil, x);
        break;
      case Builtin::MathTrunc:
        x = lowering_.builder_.CreateUnaryIntrinsic(llvm::Intrinsic::trunc, x);
        break;
      default:
        x = lowering_.builder_.CreateCall(lowering_.rt_.mathRound, {x});
        break;
      }
    }
    if (type == from) {
      return x;
    }
    if (from->isFloat()) {
      return lowering_.numberLowerer().emitCheckedInteger(x, type);
    }
    return lowering_.numberLowerer().emitCheckedIntCast(x, from, type);
  }

  switch (call->builtin) {
  case Builtin::MathAbs:
    if (type->isFloat()) {
      return lowering_.builder_.CreateUnaryIntrinsic(llvm::Intrinsic::fabs, x);
    }
    if (type->isUnsigned()) {
      return x;
    }
    return lowering_.builder_.CreateBinaryIntrinsic(llvm::Intrinsic::abs, x,
                                                    lowering_.builder_.getFalse());
  case Builtin::MathSign: {
    llvm::Type *t = x->getType();
    if (type->isFloat()) {
      llvm::Value *sign = lowering_.builder_.CreateSelect(
          lowering_.builder_.CreateFCmpOLT(x, llvm::ConstantFP::get(t, 0.0)),
          llvm::ConstantFP::get(t, -1.0), x);
      return lowering_.builder_.CreateSelect(
          lowering_.builder_.CreateFCmpOGT(x, llvm::ConstantFP::get(t, 0.0)),
          llvm::ConstantFP::get(t, 1.0), sign);
    }
    llvm::Value *zero = llvm::ConstantInt::get(t, 0);
    if (type->isUnsigned()) {
      return lowering_.builder_.CreateZExt(lowering_.builder_.CreateICmpNE(x, zero), t);
    }
    llvm::Value *sign = lowering_.builder_.CreateSelect(lowering_.builder_.CreateICmpSLT(x, zero),
                                                        llvm::ConstantInt::getSigned(t, -1), zero);
    return lowering_.builder_.CreateSelect(lowering_.builder_.CreateICmpSGT(x, zero),
                                           llvm::ConstantInt::get(t, 1), sign);
  }
  case Builtin::MathClamp: {
    llvm::Intrinsic::ID up = type->isFloat()      ? llvm::Intrinsic::maxnum
                             : type->isUnsigned() ? llvm::Intrinsic::umax
                                                  : llvm::Intrinsic::smax;
    llvm::Intrinsic::ID down = type->isFloat()      ? llvm::Intrinsic::minnum
                               : type->isUnsigned() ? llvm::Intrinsic::umin
                                                    : llvm::Intrinsic::smin;
    return lowering_.builder_.CreateBinaryIntrinsic(
        down, lowering_.builder_.CreateBinaryIntrinsic(up, args[0], args[1]), args[2]);
  }
  case Builtin::MathMin:
  case Builtin::MathMax: {
    bool isMin = call->builtin == Builtin::MathMin;
    const Type *optional = type->isOptional() ? type : nullptr;
    type = type->nonOptional();
    llvm::Intrinsic::ID id =
        type->isFloat()      ? (isMin ? llvm::Intrinsic::minimum : llvm::Intrinsic::maximum)
        : type->isUnsigned() ? (isMin ? llvm::Intrinsic::umin : llvm::Intrinsic::umax)
                             : (isMin ? llvm::Intrinsic::smin : llvm::Intrinsic::smax);
    if (!call->hasSpread()) {
      llvm::Value *result = args[0];
      for (size_t i = 1; i < args.size(); ++i) {
        result = lowering_.builder_.CreateBinaryIntrinsic(id, result, args[i]);
      }
      return result;
    }
    llvm::Value *result, *has = lowering_.builder_.getFalse();
    if (type->isFloat()) {
      result = llvm::ConstantFP::getInfinity(f64, !isMin);
    } else {
      unsigned bits = type->bitWidth();
      llvm::APInt start = type->isUnsigned()
                              ? (isMin ? llvm::APInt::getMaxValue(bits) : llvm::APInt(bits, 0))
                              : (isMin ? llvm::APInt::getSignedMaxValue(bits)
                                       : llvm::APInt::getSignedMinValue(bits));
      result = llvm::ConstantInt::get(lowering_.moduleBuilder().lower(type), start);
    }
    for (size_t i = 0; i < args.size(); ++i) {
      if (!call->spreads(i)) {
        result = lowering_.builder_.CreateBinaryIntrinsic(id, result, args[i]);
        continue;
      }
      const Type *arrayType = call->args[i]->type, *element = arrayType->inner();
      has = lowering_.builder_.CreateOr(
          has, lowering_.builder_.CreateICmpNE(lowering_.arrayLowerer().arrayLength(args[i]),
                                               lowering_.builder_.getInt64(0)));
      result = lowering_.arrayLowerer().emitArrayFold(
          args[i], element, result, [&](llvm::Value *acc, llvm::Value *value) {
            return lowering_.builder_.CreateBinaryIntrinsic(
                id, acc, lowering_.exprLowerer().emitConversion(value, element, type));
          });
      lowering_.refCounts().releaseValue(args[i], arrayType);
    }
    if (optional) {
      return lowering_.optionalLowerer().makeOptional(result, has, optional);
    }
    return result;
  }
  case Builtin::MathSqrt:
    return lowering_.builder_.CreateUnaryIntrinsic(llvm::Intrinsic::sqrt, x);
  case Builtin::MathFround:
    return lowering_.builder_.CreateFPExt(
        lowering_.builder_.CreateFPTrunc(x, lowering_.builder_.getFloatTy()), f64);
  case Builtin::MathClz32:
  case Builtin::BitLeadingZeros:
    return lowering_.builder_.CreateZExtOrTrunc(
        lowering_.builder_.CreateBinaryIntrinsic(llvm::Intrinsic::ctlz, x,
                                                 lowering_.builder_.getFalse()),
        lowering_.builder_.getInt64Ty());
  case Builtin::BitTrailingZeros:
    return lowering_.builder_.CreateZExtOrTrunc(
        lowering_.builder_.CreateBinaryIntrinsic(llvm::Intrinsic::cttz, x,
                                                 lowering_.builder_.getFalse()),
        lowering_.builder_.getInt64Ty());
  case Builtin::BitOnesCount:
    return lowering_.builder_.CreateZExtOrTrunc(
        lowering_.builder_.CreateUnaryIntrinsic(llvm::Intrinsic::ctpop, x),
        lowering_.builder_.getInt64Ty());
  case Builtin::BitReverseBytes:
    return lowering_.builder_.CreateUnaryIntrinsic(llvm::Intrinsic::bswap, x);
  case Builtin::BitRotateLeft:
  case Builtin::BitRotateRight:
    return lowering_.builder_.CreateIntrinsic(
        call->builtin == Builtin::BitRotateLeft ? llvm::Intrinsic::fshl : llvm::Intrinsic::fshr,
        {x->getType()}, {x, x, args[1]});
  case Builtin::BitWrapFrom:
    return lowering_.numberLowerer().intCast(x, call->args[0]->type, type);
  case Builtin::BitMul64:
  case Builtin::BitAdd64:
  case Builtin::BitSub64: {
    llvm::IRBuilder<> &b = lowering_.builder_;
    llvm::Type *i128 = b.getInt128Ty(), *i64 = b.getInt64Ty();
    llvm::Value *a = b.CreateZExt(args[0], i128), *c = b.CreateZExt(args[1], i128);
    llvm::Value *first, *second;
    if (call->builtin == Builtin::BitMul64) {
      llvm::Value *product = b.CreateMul(a, c);
      first = b.CreateTrunc(b.CreateLShr(product, 64), i64);
      second = b.CreateTrunc(product, i64);
    } else {
      llvm::Value *in = b.CreateZExt(b.CreateAnd(args[2], b.getInt64(1)), i128);
      llvm::Value *wide = call->builtin == Builtin::BitAdd64 ? b.CreateAdd(b.CreateAdd(a, c), in)
                                                             : b.CreateSub(b.CreateSub(a, c), in);
      first = b.CreateTrunc(wide, i64);
      second = b.CreateAnd(b.CreateTrunc(b.CreateLShr(wide, 64), i64), b.getInt64(1));
    }
    llvm::Value *object = lowering_.objectLayouts().allocObject(type, {0, 1});
    b.CreateStore(first,
                  lowering_.objectLayouts().fieldAddress(object, type, type->fields()[0].name));
    b.CreateStore(second,
                  lowering_.objectLayouts().fieldAddress(object, type, type->fields()[1].name));
    return object;
  }
  case Builtin::MathImul:
    return lowering_.builder_.CreateMul(args[0], args[1]);
  case Builtin::MathRandom:
    return lowering_.builder_.CreateCall(lowering_.rt_.mathRandom, {});
  case Builtin::MathHypot:
    if (args.empty()) {
      return llvm::ConstantFP::get(f64, 0.0);
    }
    return lowering_.builder_.CreateCall(
        lowering_.rt_.mathHypot, {spillArray(args, f64), lowering_.builder_.getInt64(args.size())});
  default:
    break;
  }

  static const std::pair<Builtin, llvm::StringLiteral> runtime[] = {
      {Builtin::MathCbrt, "cbrt"},   {Builtin::MathExp, "exp"},     {Builtin::MathExpm1, "expm1"},
      {Builtin::MathLog, "log"},     {Builtin::MathLog1p, "log1p"}, {Builtin::MathLog2, "log2"},
      {Builtin::MathLog10, "log10"}, {Builtin::MathSin, "sin"},     {Builtin::MathCos, "cos"},
      {Builtin::MathTan, "tan"},     {Builtin::MathAsin, "asin"},   {Builtin::MathAcos, "acos"},
      {Builtin::MathAtan, "atan"},   {Builtin::MathAtan2, "atan2"}, {Builtin::MathSinh, "sinh"},
      {Builtin::MathCosh, "cosh"},   {Builtin::MathTanh, "tanh"},   {Builtin::MathAsinh, "asinh"},
      {Builtin::MathAcosh, "acosh"}, {Builtin::MathAtanh, "atanh"},
  };
  for (const auto &[builtin, name] : runtime) {
    if (builtin == call->builtin) {
      return lowering_.builder_.CreateCall(mathFunction(name, static_cast<unsigned>(args.size())),
                                           args);
    }
  }
  llvm_unreachable("not a function of Math");
}

}
