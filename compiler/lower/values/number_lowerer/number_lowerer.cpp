#include "compiler/lower/values/number_lowerer/number_lowerer.h"
#include "compiler/library/decimal/decimal_lowerer/decimal_lowerer.h"
#include "compiler/library/math/math_lowerer/math_lowerer.h"
#include "compiler/library/process/process_lowerer/process_lowerer.h"
#include "compiler/library/result/result_lowerer/result_lowerer.h"
#include "compiler/lower/expr/assign_lowerer/assign_lowerer.h"
#include "compiler/lower/expr/expr_lowerer/expr_lowerer.h"
#include "compiler/lower/function_frame/function_frame.h"
#include "compiler/lower/memory/ref_counts/ref_counts.h"
#include "compiler/lower/module_builder/module_builder.h"
#include "compiler/lower/stmt/stmt_lowerer/stmt_lowerer.h"
#include "compiler/lower/values/string_lowerer/string_lowerer.h"
#include "llvm/IR/Intrinsics.h"
#include "llvm/IR/MDBuilder.h"
#include "llvm/Support/ErrorHandling.h"

using llvm::BasicBlock;
using llvm::cast;
using llvm::dyn_cast;
using llvm::isa;
using llvm::Value;

namespace tz {

NumberLowerer::NumberLowerer(Lowering &lowering) : lowering_(lowering) {}

llvm::Value *NumberLowerer::toI64(llvm::Value *value, const Type *type) {
  llvm::Type *i64 = lowering_.builder_.getInt64Ty();

  return type->isUnsigned() ? lowering_.builder_.CreateZExtOrTrunc(value, i64)
                            : lowering_.builder_.CreateSExtOrTrunc(value, i64);
}

llvm::Value *NumberLowerer::emitCheckedIntCast(llvm::Value *value, const Type *from,
                                               const Type *to) {
  if (to->holds(from)) {
    return intCast(value, from, to);
  }
  llvm::IRBuilder<> &b = lowering_.builder_;
  llvm::Type *i64 = b.getInt64Ty();
  llvm::Value *wide = toI64(value, from);
  llvm::Value *fits = b.getTrue();
  if (from->isUnsigned()) {
    fits = b.CreateICmpULE(wide, b.getInt64(to->intMax()));
  } else {
    if (to->intMin() > from->intMin()) {
      fits = b.CreateICmpSGE(wide, llvm::ConstantInt::getSigned(i64, to->intMin()));
    }
    if (to->intMax() < from->intMax()) {
      fits = b.CreateAnd(fits, b.CreateICmpSLE(wide, b.getInt64(to->intMax())));
    }
  }
  llvm::BasicBlock *badBlock = lowering_.functionFrame().newBlock("int.does.not.fit");
  llvm::BasicBlock *okBlock = lowering_.functionFrame().newBlock("int.fits");
  b.CreateCondBr(fits, okBlock, badBlock,
                 llvm::MDBuilder(lowering_.ctx_).createLikelyBranchWeights());
  lowering_.functionFrame().switchTo(badBlock);
  b.CreateCall(lowering_.processLowerer().processFunction(
                   "tz_panic_int_does_not_fit", b.getVoidTy(), {i64, b.getInt32Ty(), b.getPtrTy()}),
               {wide, b.getInt32(from->isUnsigned() ? 1 : 0), b.CreateGlobalString(to->name())});
  b.CreateUnreachable();
  lowering_.functionFrame().switchTo(okBlock);

  return intCast(value, from, to);
}

llvm::Value *NumberLowerer::intCast(llvm::Value *value, const Type *from, const Type *to) {
  llvm::Type *target = lowering_.moduleBuilder().lower(to);

  return from->isUnsigned() ? lowering_.builder_.CreateZExtOrTrunc(value, target)
                            : lowering_.builder_.CreateSExtOrTrunc(value, target);
}

llvm::Value *NumberLowerer::toDouble(llvm::Value *value, const Type *from) {
  llvm::Type *f64 = lowering_.builder_.getDoubleTy();

  return from->isUnsigned() ? lowering_.builder_.CreateUIToFP(value, f64)
                            : lowering_.builder_.CreateSIToFP(value, f64);
}

llvm::Value *NumberLowerer::fromDouble(llvm::Value *value, const Type *to) {
  llvm::Type *target = lowering_.moduleBuilder().lower(to);

  return to->isUnsigned() ? lowering_.builder_.CreateFPToUI(value, target)
                          : lowering_.builder_.CreateFPToSI(value, target);
}

llvm::Value *NumberLowerer::emitCheckedInteger(llvm::Value *value, const Type *to) {
  llvm::Type *f64 = lowering_.builder_.getDoubleTy();
  double lowest = double(to->intMin());
  double above = to->bitWidth() == 64
                     ? (to->isUnsigned() ? 18446744073709551616.0 : 9223372036854775808.0)
                     : double(to->intMax()) + 1;
  llvm::Value *low = llvm::ConstantFP::get(f64, lowest);
  llvm::Value *high = llvm::ConstantFP::get(f64, above);
  llvm::Value *fits = lowering_.builder_.CreateAnd(lowering_.builder_.CreateFCmpOGE(value, low),
                                                   lowering_.builder_.CreateFCmpOLT(value, high));
  llvm::BasicBlock *badBlock = lowering_.functionFrame().newBlock("int.does.not.fit");
  llvm::BasicBlock *okBlock = lowering_.functionFrame().newBlock("int.fits");
  lowering_.builder_.CreateCondBr(fits, okBlock, badBlock,
                                  llvm::MDBuilder(lowering_.ctx_).createLikelyBranchWeights());
  lowering_.functionFrame().switchTo(badBlock);
  lowering_.builder_.CreateCall(lowering_.processLowerer().processFunction(
                                    "tz_panic_does_not_fit", lowering_.builder_.getVoidTy(),
                                    {f64, lowering_.builder_.getPtrTy()}),
                                {value, lowering_.builder_.CreateGlobalString(to->name())});
  lowering_.builder_.CreateUnreachable();
  lowering_.functionFrame().switchTo(okBlock);

  return fromDouble(value, to);
}

llvm::Value *NumberLowerer::emitStringToNumber(llvm::Value *str) {
  return lowering_.builder_.CreateCall(
      lowering_.rt_.strToNumber,
      {lowering_.stringLowerer().stringObj(str), lowering_.stringLowerer().stringMeta(str)});
}

llvm::Value *NumberLowerer::emitNumberBuiltin(CallExpr *call) {
  llvm::Type *f64 = lowering_.builder_.getDoubleTy();
  bool hasArg = !call->args.empty();
  const Type *argType = hasArg ? call->args[0]->type : nullptr;

  switch (call->builtin) {
  case Builtin::NumberOf: {
    if (!hasArg) {
      return llvm::ConstantFP::get(f64, 0.0);
    }
    Lowering::Operand value = lowering_.refCounts().emitOperand(call->args[0], true);
    const Type *base = argType;
    llvm::Value *v = value.value;
    llvm::Type *i64 = lowering_.builder_.getInt64Ty();
    if (base->isString() && call->type->isResult()) {
      llvm::Value *parsed = lowering_.builder_.CreateCall(
          lowering_.processLowerer().processFunction(
              "tz_str_to_number_r", f64,
              {lowering_.builder_.getPtrTy(), i64, lowering_.builder_.getPtrTy()}),
          {lowering_.stringLowerer().stringObj(v), lowering_.stringLowerer().stringMeta(v),
           lowering_.resultLowerer().failureMaker(call->type->errType())});
      lowering_.refCounts().dropOperand(value);
      return lowering_.resultLowerer().catchFailure(parsed, call->type, call->loc());
    }
    llvm::Value *number =
        base->isString() ? emitStringToNumber(v)
        : base->isBool() ? lowering_.builder_.CreateUIToFP(v, f64)
        : base->isDecimal()
            ? lowering_.builder_.CreateCall(
                  lowering_.decimalLowerer().decimalFunction("tz_dec_to_f64", f64, {i64, i64}),
                  {lowering_.decimalLowerer().decimalCoef(v),
                   lowering_.decimalLowerer().decimalMeta(v)})
        : base->isInteger() ? toDouble(v, base)
                            : v;
    lowering_.refCounts().dropOperand(value);
    return number;
  }
  case Builtin::ParseInt:
  case Builtin::ParseFloat: {
    bool isInt = call->builtin == Builtin::ParseInt;
    Lowering::Operand str = lowering_.refCounts().emitOperand(
        call->args[0], call->args.size() == 1 || AssignLowerer::cannotAssign(call->args[1]));
    llvm::Type *ptr = lowering_.builder_.getPtrTy(), *i64 = lowering_.builder_.getInt64Ty();
    llvm::Value *maker = lowering_.resultLowerer().failureMaker(call->type->errType());
    llvm::Value *result;
    if (isInt) {
      llvm::Value *radix = call->args.size() > 1 ? lowering_.exprLowerer().emitExpr(call->args[1])
                                                 : lowering_.builder_.getInt64(0);
      result = lowering_.builder_.CreateCall(
          lowering_.processLowerer().processFunction("tz_parse_int_r", i64, {ptr, i64, i64, ptr}),
          {lowering_.stringLowerer().stringObj(str.value),
           lowering_.stringLowerer().stringMeta(str.value), radix, maker});
    } else {
      result = lowering_.builder_.CreateCall(
          lowering_.processLowerer().processFunction("tz_parse_float_r", f64, {ptr, i64, ptr}),
          {lowering_.stringLowerer().stringObj(str.value),
           lowering_.stringLowerer().stringMeta(str.value), maker});
    }
    lowering_.refCounts().dropOperand(str);
    return lowering_.resultLowerer().catchFailure(result, call->type, call->loc());
  }
  case Builtin::IsInteger:
  case Builtin::IsSafeInteger: {
    llvm::Value *x = lowering_.exprLowerer().emitExpr(call->args[0]);
    if (argType->isInteger() && argType->bitWidth() < 64) {
      return lowering_.builder_.getTrue();
    }
    if (argType->isInteger()) {
      if (call->builtin != Builtin::IsSafeInteger) {
        return lowering_.builder_.getTrue();
      }
      llvm::Value *limit = lowering_.builder_.getInt64((int64_t(1) << 53) - 1);
      if (argType->isUnsigned()) {
        return lowering_.builder_.CreateICmpULE(x, limit);
      }
      return lowering_.builder_.CreateICmpULE(lowering_.builder_.CreateAdd(x, limit),
                                              lowering_.builder_.CreateAdd(limit, limit));
    }
    llvm::Value *magnitude = lowering_.builder_.CreateUnaryIntrinsic(llvm::Intrinsic::fabs, x);
    llvm::Value *finite =
        lowering_.builder_.CreateFCmpOLT(magnitude, llvm::ConstantFP::getInfinity(f64));
    llvm::Value *whole = lowering_.builder_.CreateFCmpOEQ(
        lowering_.builder_.CreateUnaryIntrinsic(llvm::Intrinsic::trunc, x), x);
    llvm::Value *integer = lowering_.builder_.CreateAnd(finite, whole);
    if (call->builtin == Builtin::IsInteger) {
      return integer;
    }
    llvm::Value *safe =
        lowering_.builder_.CreateFCmpOLE(magnitude, llvm::ConstantFP::get(f64, 9007199254740991.0));
    return lowering_.builder_.CreateAnd(integer, safe);
  }
  case Builtin::FromCharCode:
  case Builtin::FromCodePoint: {
    if (!hasArg) {
      return lowering_.stringLowerer().stringLiteral(u"");
    }
    bool units = call->builtin == Builtin::FromCharCode;
    std::vector<llvm::Value *> values;
    for (Expr *arg : call->args) {
      llvm::Value *value = lowering_.exprLowerer().emitExpr(arg);
      values.push_back(
          units ? lowering_.builder_.CreateTrunc(value, lowering_.builder_.getInt32Ty()) : value);
    }
    llvm::Value *array =
        lowering_.mathLowerer().spillArray(values, units ? lowering_.builder_.getInt32Ty() : f64);
    llvm::Value *text = lowering_.stringLowerer().callReturningString(
        units ? lowering_.rt_.strFromCharCodes : lowering_.rt_.strFromCodePoints,
        {array, lowering_.builder_.getInt64(values.size())});
    return text;
  }
  case Builtin::UriEncodeComponent:
  case Builtin::UriEncode:
  case Builtin::UriDecodeComponent:
  case Builtin::UriDecode: {
    llvm::StringRef name = call->builtin == Builtin::UriEncodeComponent ? "tz_uri_encode_component"
                           : call->builtin == Builtin::UriEncode        ? "tz_uri_encode"
                           : call->builtin == Builtin::UriDecodeComponent
                               ? "tz_uri_decode_component"
                               : "tz_uri_decode";
    llvm::Type *ptr = lowering_.builder_.getPtrTy();
    Lowering::Operand text = lowering_.refCounts().emitOperand(call->args[0], true);
    if (call->type->isResult()) {
      llvm::Value *result = lowering_.stringLowerer().callReturningString(
          lowering_.processLowerer().processFunction(
              name, lowering_.builder_.getVoidTy(),
              {ptr, ptr, lowering_.builder_.getInt64Ty(), ptr}),
          {lowering_.stringLowerer().stringObj(text.value),
           lowering_.stringLowerer().stringMeta(text.value),
           lowering_.resultLowerer().failureMaker(call->type->errType())});
      lowering_.refCounts().dropOperand(text);
      return lowering_.resultLowerer().catchFailure(result, call->type, call->loc());
    }
    llvm::Value *result = lowering_.stringLowerer().callReturningString(
        lowering_.processLowerer().processFunction(name, lowering_.builder_.getVoidTy(),
                                                   {ptr, ptr, lowering_.builder_.getInt64Ty()}),
        {lowering_.stringLowerer().stringObj(text.value),
         lowering_.stringLowerer().stringMeta(text.value)});
    lowering_.refCounts().dropOperand(text);
    return result;
  }
  case Builtin::NumToFixed: {
    llvm::Value *x = lowering_.exprLowerer().emitExpr(cast<MemberExpr>(call->callee)->object);
    llvm::Value *digits =
        hasArg ? lowering_.exprLowerer().emitExpr(call->args[0]) : lowering_.builder_.getInt64(0);
    llvm::Value *text =
        lowering_.stringLowerer().callReturningString(lowering_.rt_.numToFixed, {x, digits});
    return text;
  }
  case Builtin::NumToRadix: {
    llvm::Value *x = lowering_.exprLowerer().emitExpr(cast<MemberExpr>(call->callee)->object);
    llvm::Value *radix = lowering_.exprLowerer().emitExpr(call->args[0]);
    llvm::Value *text =
        lowering_.stringLowerer().callReturningString(lowering_.rt_.numToRadix, {x, radix});
    return text;
  }
  case Builtin::NumConvert: {
    Expr *object = cast<MemberExpr>(call->callee)->object;
    llvm::Value *x = lowering_.exprLowerer().emitExpr(object);
    const Type *from = object->type, *to = call->type;
    if (from == to) {
      return x;
    }
    if (to->isDecimal()) {
      return lowering_.decimalLowerer().decimalFromInteger(x, from);
    }
    if (to->isFloat()) {
      return toDouble(x, from);
    }
    if (from->isFloat()) {
      return call->rangeProven ? fromDouble(x, to) : emitCheckedInteger(x, to);
    }
    return call->rangeProven ? intCast(x, from, to) : emitCheckedIntCast(x, from, to);
  }
  default:
    llvm_unreachable("not a function of Number");
  }
}

}
