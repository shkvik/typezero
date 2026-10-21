// The functions of Math and Number, parseInt and the like, String.fromCharCode
// and x.toFixed(). Simple ones are emitted inline; the rest call the runtime
// (runtime/math.c and runtime/number.c).

#include "codegen/codegen.h"

#include "llvm/IR/Intrinsics.h"
#include "llvm/Support/ErrorHandling.h"

using llvm::cast;

namespace tz {

llvm::FunctionCallee CodeGen::mathFunction(llvm::StringRef name, unsigned params) {
  llvm::Type *f64 = builder_.getDoubleTy();
  std::vector<llvm::Type *> types(params, f64);
  llvm::FunctionCallee callee =
      module_->getOrInsertFunction(("tz_math_" + name).str(), llvm::FunctionType::get(f64, types, false));
  cast<llvm::Function>(callee.getCallee())->addFnAttr(llvm::Attribute::NoUnwind);
  return callee;
}

llvm::Value *CodeGen::spillArray(llvm::ArrayRef<llvm::Value *> values, llvm::Type *element) {
  auto *arrayTy = llvm::ArrayType::get(element, values.size());
  llvm::AllocaInst *array = createStackSlot(arrayTy, "args");
  for (size_t i = 0; i < values.size(); ++i)
    builder_.CreateStore(values[i],
                         builder_.CreateConstInBoundsGEP2_32(arrayTy, array, 0, static_cast<unsigned>(i)));
  return array;
}

llvm::Value *CodeGen::emitCheckedInteger(llvm::Value *value, const Type *to) {
  return builder_.CreateCall(to->isInt() ? rt_.checkedInt32 : rt_.checkedInt64, {value});
}

llvm::Value *CodeGen::emitStringToNumber(llvm::Value *str) {
  return builder_.CreateCall(rt_.strToNumber, {stringObj(str), stringMeta(str)});
}

// Sema made the arguments doubles where the function takes doubles, ints for
// clz32 and imul, and the common type for abs, sign, min and max.
llvm::Value *CodeGen::emitMathCall(CallExpr *call) {
  std::vector<llvm::Value *> args;
  for (Expr *arg : call->args)
    args.push_back(emitExpr(arg));
  const Type *type = call->type;
  llvm::Type *f64 = builder_.getDoubleTy();
  llvm::Value *x = args.empty() ? nullptr : args[0];

  if (isRoundingBuiltin(call->builtin)) {
    // An integer is whole already. Sema typed the call as an integer where
    // one is expected: then the result must fit, or the program stops.
    const Type *from = call->args[0]->type;
    if (from->isFloat()) {
      switch (call->builtin) {
      case Builtin::MathFloor:
        x = builder_.CreateUnaryIntrinsic(llvm::Intrinsic::floor, x);
        break;
      case Builtin::MathCeil:
        x = builder_.CreateUnaryIntrinsic(llvm::Intrinsic::ceil, x);
        break;
      case Builtin::MathTrunc:
        x = builder_.CreateUnaryIntrinsic(llvm::Intrinsic::trunc, x);
        break;
      default: // JavaScript rounds halves up, which no intrinsic does
        x = builder_.CreateCall(rt_.mathRound, {x});
        break;
      }
    }
    if (type == from)
      return x;
    if (from->isFloat())
      return emitCheckedInteger(x, type);
    return builder_.CreateCall(rt_.checkedInt32OfLong, {x});
  }

  switch (call->builtin) {
  case Builtin::MathAbs:
    if (type->isFloat())
      return builder_.CreateUnaryIntrinsic(llvm::Intrinsic::fabs, x);
    // The smallest int stays as it is, as `-x` does.
    return builder_.CreateBinaryIntrinsic(llvm::Intrinsic::abs, x, builder_.getFalse());
  case Builtin::MathSign: {
    llvm::Type *t = x->getType();
    if (type->isFloat()) { // NaN, 0 and -0 come back as they are
      llvm::Value *sign = builder_.CreateSelect(builder_.CreateFCmpOLT(x, llvm::ConstantFP::get(t, 0.0)),
                                                llvm::ConstantFP::get(t, -1.0), x);
      return builder_.CreateSelect(builder_.CreateFCmpOGT(x, llvm::ConstantFP::get(t, 0.0)),
                                   llvm::ConstantFP::get(t, 1.0), sign);
    }
    llvm::Value *zero = llvm::ConstantInt::get(t, 0);
    llvm::Value *sign = builder_.CreateSelect(builder_.CreateICmpSLT(x, zero),
                                              llvm::ConstantInt::getSigned(t, -1), zero);
    return builder_.CreateSelect(builder_.CreateICmpSGT(x, zero), llvm::ConstantInt::get(t, 1), sign);
  }
  case Builtin::MathMin:
  case Builtin::MathMax: {
    bool isMin = call->builtin == Builtin::MathMin;
    if (args.empty())
      return llvm::ConstantFP::getInfinity(f64, /*Negative=*/!isMin);
    // minimum and maximum give NaN if either side is NaN, and order -0
    // before 0, as JavaScript does.
    llvm::Intrinsic::ID id = type->isFloat() ? (isMin ? llvm::Intrinsic::minimum : llvm::Intrinsic::maximum)
                                             : (isMin ? llvm::Intrinsic::smin : llvm::Intrinsic::smax);
    llvm::Value *result = args[0];
    for (size_t i = 1; i < args.size(); ++i)
      result = builder_.CreateBinaryIntrinsic(id, result, args[i]);
    return result;
  }
  case Builtin::MathSqrt:
    return builder_.CreateUnaryIntrinsic(llvm::Intrinsic::sqrt, x);
  case Builtin::MathFround: // to the nearest float
    return builder_.CreateFPExt(builder_.CreateFPTrunc(x, builder_.getFloatTy()), f64);
  case Builtin::MathClz32:
    return builder_.CreateBinaryIntrinsic(llvm::Intrinsic::ctlz, x, builder_.getFalse());
  case Builtin::MathImul:
    return builder_.CreateMul(args[0], args[1]);
  case Builtin::MathRandom:
    return builder_.CreateCall(rt_.mathRandom, {});
  case Builtin::MathHypot:
    if (args.empty())
      return llvm::ConstantFP::get(f64, 0.0);
    return builder_.CreateCall(rt_.mathHypot,
                               {spillArray(args, f64), builder_.getInt64(args.size())});
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
  for (const auto &[builtin, name] : runtime)
    if (builtin == call->builtin)
      return builder_.CreateCall(mathFunction(name, static_cast<unsigned>(args.size())), args);
  llvm_unreachable("not a function of Math");
}

llvm::Value *CodeGen::emitNumberBuiltin(CallExpr *call) {
  llvm::Type *f64 = builder_.getDoubleTy();
  llvm::Value *nan = llvm::ConstantFP::getNaN(f64);
  bool hasArg = !call->args.empty();
  const Type *argType = hasArg ? call->args[0]->type : nullptr;

  switch (call->builtin) {
  case Builtin::NumberOf: {
    if (!hasArg)
      return llvm::ConstantFP::get(f64, 0.0);
    if (argType->isUndefined()) {
      emitDiscarded(call->args[0]);
      return nan;
    }
    Operand value = emitOperand(call->args[0], /*canBorrow=*/true);
    const Type *base = argType->nonOptional();
    llvm::Value *v = argType->isOptional() ? builder_.CreateExtractValue(value.value, 0) : value.value;
    llvm::Value *number = base->isString() ? emitStringToNumber(v)
                          : base->isBool() ? builder_.CreateUIToFP(v, f64)
                                           : v; // Sema made numbers doubles
    if (argType->isOptional())
      number = builder_.CreateSelect(builder_.CreateExtractValue(value.value, 1), number, nan);
    dropOperand(value);
    return number;
  }
  case Builtin::ParseInt:
  case Builtin::ParseFloat: {
    bool isInt = call->builtin == Builtin::ParseInt;
    Operand str = emitOperand(call->args[0], call->args.size() == 1 || cannotAssign(call->args[1]));
    llvm::Value *result;
    if (isInt) {
      // A radix of 0 means none, also when it is given.
      llvm::Value *radix = call->args.size() > 1 ? emitExpr(call->args[1]) : builder_.getInt32(0);
      result = builder_.CreateCall(rt_.parseInt, {stringObj(str.value), stringMeta(str.value), radix});
    } else {
      result = builder_.CreateCall(rt_.parseFloat, {stringObj(str.value), stringMeta(str.value)});
    }
    dropOperand(str);
    // parseInt where an int is expected: the number must fit, NaN does not.
    return call->type->isInteger() ? emitCheckedInteger(result, call->type) : result;
  }
  case Builtin::IsNaN:
  case Builtin::IsFinite:
  case Builtin::IsInteger:
  case Builtin::IsSafeInteger: {
    llvm::Value *x = emitExpr(call->args[0]);
    if (argType->isInt())
      return call->builtin == Builtin::IsNaN ? builder_.getFalse() : builder_.getTrue();
    if (argType->isInteger()) { // a long: only isSafeInteger can be false
      if (call->builtin == Builtin::IsNaN)
        return builder_.getFalse();
      if (call->builtin != Builtin::IsSafeInteger)
        return builder_.getTrue();
      llvm::Value *limit = builder_.getInt64((int64_t(1) << 53) - 1);
      return builder_.CreateICmpULE(builder_.CreateAdd(x, limit), builder_.CreateAdd(limit, limit));
    }
    if (call->builtin == Builtin::IsNaN)
      return builder_.CreateFCmpUNO(x, x);
    llvm::Value *magnitude = builder_.CreateUnaryIntrinsic(llvm::Intrinsic::fabs, x);
    llvm::Value *finite =
        builder_.CreateFCmpOLT(magnitude, llvm::ConstantFP::getInfinity(f64));
    if (call->builtin == Builtin::IsFinite)
      return finite;
    llvm::Value *whole = builder_.CreateFCmpOEQ(builder_.CreateUnaryIntrinsic(llvm::Intrinsic::trunc, x), x);
    llvm::Value *integer = builder_.CreateAnd(finite, whole);
    if (call->builtin == Builtin::IsInteger)
      return integer;
    llvm::Value *safe = builder_.CreateFCmpOLE(magnitude, llvm::ConstantFP::get(f64, 9007199254740991.0));
    return builder_.CreateAnd(integer, safe);
  }
  case Builtin::FromCharCode:
  case Builtin::FromCodePoint: {
    if (!hasArg)
      return stringLiteral(u"");
    bool units = call->builtin == Builtin::FromCharCode;
    std::vector<llvm::Value *> values;
    for (Expr *arg : call->args)
      values.push_back(emitExpr(arg));
    llvm::Value *array = spillArray(values, units ? builder_.getInt32Ty() : f64);
    return callReturningString(units ? rt_.strFromCharCodes : rt_.strFromCodePoints,
                               {array, builder_.getInt64(values.size())});
  }
  case Builtin::NumToFixed: {
    llvm::Value *x = emitExpr(cast<MemberExpr>(call->callee)->object);
    llvm::Value *digits = hasArg ? emitExpr(call->args[0]) : builder_.getInt64(0);
    return callReturningString(rt_.numToFixed, {x, digits});
  }
  default:
    llvm_unreachable("not a function of Number");
  }
}

} // namespace tz
