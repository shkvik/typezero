#include "compiler/library/math/math_lowerer/math_lowerer.h"
#include "compiler/lower/module_builder/module_builder.h"
#include "compiler/lower/function_frame/function_frame.h"
#include "compiler/lower/memory/ref_counts/ref_counts.h"
#include "compiler/lower/expr/expr_lowerer/expr_lowerer.h"
#include "compiler/lower/values/array_lowerer/array_lowerer.h"
#include "compiler/lower/values/number_lowerer/number_lowerer.h"
#include "compiler/lower/errors/throw_lowerer/throw_lowerer.h"
#include "llvm/IR/Intrinsics.h"
#include "llvm/IR/MDBuilder.h"
#include "llvm/Support/ErrorHandling.h"

using llvm::cast;
using llvm::dyn_cast;
using llvm::isa;
using llvm::BasicBlock;
using llvm::Value;

namespace tz {

MathLowerer::MathLowerer(CodeGen &codegen) : codegen_(codegen) {}

llvm::FunctionCallee MathLowerer::mathFunction(llvm::StringRef name, unsigned params) {
  llvm::Type *f64 = codegen_.builder_.getDoubleTy();
  std::vector<llvm::Type *> types(params, f64);
  llvm::FunctionCallee callee = codegen_.module_->getOrInsertFunction(
      ("tz_math_" + name).str(), llvm::FunctionType::get(f64, types, false));
  cast<llvm::Function>(callee.getCallee())->addFnAttr(llvm::Attribute::NoUnwind);

  return callee;
}

llvm::Value *MathLowerer::spillArray(llvm::ArrayRef<llvm::Value *> values, llvm::Type *element) {
  auto *arrayTy = llvm::ArrayType::get(element, values.size());
  llvm::AllocaInst *array = codegen_.functionFrame().createStackSlot(arrayTy, "args");
  for (size_t i = 0; i < values.size(); ++i) {
    codegen_.builder_.CreateStore(values[i], codegen_.builder_.CreateConstInBoundsGEP2_32(arrayTy, array, 0,
                                                                        static_cast<unsigned>(i)));
  }

  return array;
}

// Sema made the arguments doubles where the function takes doubles, ints for
// clz32 and imul, and the common type for abs, sign, min and max.
llvm::Value *MathLowerer::emitMathCall(CallExpr *call) {
  std::vector<llvm::Value *> args;
  for (Expr *arg : call->args) {
    args.push_back(codegen_.exprLowerer().emitExpr(arg));
  }
  const Type *type = call->type;
  llvm::Type *f64 = codegen_.builder_.getDoubleTy();
  llvm::Value *x = args.empty() ? nullptr : args[0];

  if (isRoundingBuiltin(call->builtin)) {
    // An integer is whole already. Sema typed the call as an integer where
    // one is expected: then the result must fit, or the program stops.
    const Type *from = call->args[0]->type;
    if (from->isFloat()) {
      switch (call->builtin) {
      case Builtin::MathFloor:
        x = codegen_.builder_.CreateUnaryIntrinsic(llvm::Intrinsic::floor, x);
        break;
      case Builtin::MathCeil:
        x = codegen_.builder_.CreateUnaryIntrinsic(llvm::Intrinsic::ceil, x);
        break;
      case Builtin::MathTrunc:
        x = codegen_.builder_.CreateUnaryIntrinsic(llvm::Intrinsic::trunc, x);
        break;
      default: // JavaScript rounds halves up, which no intrinsic does
        x = codegen_.builder_.CreateCall(codegen_.rt_.mathRound, {x});
        break;
      }
    }
    if (type == from) {
      return x;
    }
    if (from->isFloat()) {
      return codegen_.numberLowerer().emitCheckedInteger(x, type);
    }
    // A long where an int is expected.
    llvm::Value *fits = codegen_.builder_.CreateICmpULE(codegen_.builder_.CreateAdd(x, codegen_.builder_.getInt64(0x80000000)),
                                               codegen_.builder_.getInt64(0xFFFFFFFF));
    llvm::BasicBlock *badBlock = codegen_.functionFrame().newBlock("int.does.not.fit");
    llvm::BasicBlock *okBlock = codegen_.functionFrame().newBlock("int.fits");
    codegen_.builder_.CreateCondBr(fits, okBlock, badBlock,
                          llvm::MDBuilder(codegen_.ctx_).createLikelyBranchWeights());
    codegen_.functionFrame().switchTo(badBlock);
    codegen_.builder_.CreateCall(codegen_.rt_.checkedInt32OfLong, {x});
    codegen_.throwLowerer().emitRuntimeThrow();
    codegen_.functionFrame().switchTo(okBlock);
    return codegen_.builder_.CreateTrunc(x, codegen_.builder_.getInt32Ty());
  }

  switch (call->builtin) {
  case Builtin::MathAbs:
    if (type->isFloat()) {
      return codegen_.builder_.CreateUnaryIntrinsic(llvm::Intrinsic::fabs, x);
    }
    // The smallest int stays as it is, as `-x` does.
    return codegen_.builder_.CreateBinaryIntrinsic(llvm::Intrinsic::abs, x, codegen_.builder_.getFalse());
  case Builtin::MathSign: {
    llvm::Type *t = x->getType();
    if (type->isFloat()) { // NaN, 0 and -0 come back as they are
      llvm::Value *sign =
          codegen_.builder_.CreateSelect(codegen_.builder_.CreateFCmpOLT(x, llvm::ConstantFP::get(t, 0.0)),
                                llvm::ConstantFP::get(t, -1.0), x);
      return codegen_.builder_.CreateSelect(codegen_.builder_.CreateFCmpOGT(x, llvm::ConstantFP::get(t, 0.0)),
                                   llvm::ConstantFP::get(t, 1.0), sign);
    }
    llvm::Value *zero = llvm::ConstantInt::get(t, 0);
    llvm::Value *sign = codegen_.builder_.CreateSelect(codegen_.builder_.CreateICmpSLT(x, zero),
                                              llvm::ConstantInt::getSigned(t, -1), zero);
    return codegen_.builder_.CreateSelect(codegen_.builder_.CreateICmpSGT(x, zero), llvm::ConstantInt::get(t, 1),
                                 sign);
  }
  case Builtin::MathMin:
  case Builtin::MathMax: {
    bool isMin = call->builtin == Builtin::MathMin;
    if (args.empty()) {
      return llvm::ConstantFP::getInfinity(f64, /*Negative=*/!isMin);
    }
    // minimum and maximum give NaN if either side is NaN, and order -0
    // before 0, as JavaScript does.
    llvm::Intrinsic::ID id = type->isFloat()
                                 ? (isMin ? llvm::Intrinsic::minimum : llvm::Intrinsic::maximum)
                                 : (isMin ? llvm::Intrinsic::smin : llvm::Intrinsic::smax);
    if (!call->hasSpread()) {
      llvm::Value *result = args[0];
      for (size_t i = 1; i < args.size(); ++i) {
        result = codegen_.builder_.CreateBinaryIntrinsic(id, result, args[i]);
      }
      return result;
    }
    // `Math.max(...a)`: from the value that changes nothing, ±Infinity (or
    // the extreme integer, where Sema proved some value is there), over the
    // arguments and the elements of the arrays, in order.
    llvm::Value *result;
    if (type->isFloat()) {
      result = llvm::ConstantFP::getInfinity(f64, /*Negative=*/!isMin);
    } else {
      result = isMin ? llvm::ConstantInt::get(codegen_.moduleBuilder().lower(type),
                                              llvm::APInt::getSignedMaxValue(type->bitWidth()))
                     : llvm::ConstantInt::get(codegen_.moduleBuilder().lower(type),
                                              llvm::APInt::getSignedMinValue(type->bitWidth()));
    }
    for (size_t i = 0; i < args.size(); ++i) {
      if (!call->spreads(i)) {
        result = codegen_.builder_.CreateBinaryIntrinsic(id, result, args[i]);
        continue;
      }
      const Type *arrayType = call->args[i]->type, *element = arrayType->inner();
      result = codegen_.arrayLowerer().emitArrayFold(args[i], element, result, [&](llvm::Value *acc, llvm::Value *value) {
        return codegen_.builder_.CreateBinaryIntrinsic(id, acc, codegen_.exprLowerer().emitConversion(value, element, type));
      });
      codegen_.refCounts().releaseValue(args[i], arrayType);
    }
    return result;
  }
  case Builtin::MathSqrt:
    return codegen_.builder_.CreateUnaryIntrinsic(llvm::Intrinsic::sqrt, x);
  case Builtin::MathFround: // to the nearest float
    return codegen_.builder_.CreateFPExt(codegen_.builder_.CreateFPTrunc(x, codegen_.builder_.getFloatTy()), f64);
  case Builtin::MathClz32:
    return codegen_.builder_.CreateBinaryIntrinsic(llvm::Intrinsic::ctlz, x, codegen_.builder_.getFalse());
  case Builtin::MathImul:
    return codegen_.builder_.CreateMul(args[0], args[1]);
  case Builtin::MathRandom:
    return codegen_.builder_.CreateCall(codegen_.rt_.mathRandom, {});
  case Builtin::MathHypot:
    if (args.empty()) {
      return llvm::ConstantFP::get(f64, 0.0);
    }
    return codegen_.builder_.CreateCall(codegen_.rt_.mathHypot,
                               {spillArray(args, f64), codegen_.builder_.getInt64(args.size())});
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
      return codegen_.builder_.CreateCall(mathFunction(name, static_cast<unsigned>(args.size())), args);
    }
  }
  llvm_unreachable("not a function of Math");
}

} // namespace tz
