#include "compiler/lower/values/number_lowerer/number_lowerer.h"
#include "compiler/library/decimal/decimal_lowerer/decimal_lowerer.h"
#include "compiler/library/math/math_lowerer/math_lowerer.h"
#include "compiler/library/process/process_lowerer/process_lowerer.h"
#include "compiler/lower/errors/throw_lowerer/throw_lowerer.h"
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

llvm::Value *NumberLowerer::emitCheckedInteger(llvm::Value *value, const Type *to) {
  bool isInt = to->isInt();
  llvm::Type *f64 = lowering_.builder_.getDoubleTy();
  llvm::Value *low = llvm::ConstantFP::get(f64, isInt ? -2147483648.0 : -9223372036854775808.0);
  llvm::Value *high = llvm::ConstantFP::get(f64, isInt ? 2147483648.0 : 9223372036854775808.0);
  llvm::Value *fits = lowering_.builder_.CreateAnd(lowering_.builder_.CreateFCmpOGE(value, low),
                                                   lowering_.builder_.CreateFCmpOLT(value, high));
  llvm::BasicBlock *badBlock = lowering_.functionFrame().newBlock("int.does.not.fit");
  llvm::BasicBlock *okBlock = lowering_.functionFrame().newBlock("int.fits");
  lowering_.builder_.CreateCondBr(fits, okBlock, badBlock,
                                  llvm::MDBuilder(lowering_.ctx_).createLikelyBranchWeights());
  lowering_.functionFrame().switchTo(badBlock);
  lowering_.builder_.CreateCall(lowering_.processLowerer().processFunction(
                                    "tz_throw_does_not_fit", lowering_.builder_.getVoidTy(),
                                    {f64, lowering_.builder_.getInt32Ty()}),
                                {value, lowering_.builder_.getInt32(isInt ? 32 : 64)});
  lowering_.throwLowerer().emitRuntimeThrow();
  lowering_.functionFrame().switchTo(okBlock);

  return lowering_.builder_.CreateFPToSI(value, lowering_.moduleBuilder().lower(to));
}

llvm::Value *NumberLowerer::emitStringToNumber(llvm::Value *str) {
  return lowering_.builder_.CreateCall(
      lowering_.rt_.strToNumber,
      {lowering_.stringLowerer().stringObj(str), lowering_.stringLowerer().stringMeta(str)});
}

llvm::Value *NumberLowerer::emitNumberBuiltin(CallExpr *call) {
  llvm::Type *f64 = lowering_.builder_.getDoubleTy();
  llvm::Value *nan = llvm::ConstantFP::getNaN(f64);
  bool hasArg = !call->args.empty();
  const Type *argType = hasArg ? call->args[0]->type : nullptr;

  switch (call->builtin) {
  case Builtin::NumberOf: {
    if (!hasArg) {
      return llvm::ConstantFP::get(f64, 0.0);
    }
    if (argType->isUndefined()) {
      lowering_.stmtLowerer().emitDiscarded(call->args[0]);
      return nan;
    }
    Lowering::Operand value = lowering_.refCounts().emitOperand(call->args[0], true);
    const Type *base = argType->nonOptional();
    llvm::Value *v =
        argType->isOptional() ? lowering_.builder_.CreateExtractValue(value.value, 0) : value.value;
    llvm::Type *i64 = lowering_.builder_.getInt64Ty();
    llvm::Value *number =
        base->isString() ? emitStringToNumber(v)
        : base->isBool() ? lowering_.builder_.CreateUIToFP(v, f64)
        : base->isDecimal()
            ? lowering_.builder_.CreateCall(
                  lowering_.decimalLowerer().decimalFunction("tz_dec_to_f64", f64, {i64, i64}),
                  {lowering_.decimalLowerer().decimalCoef(v),
                   lowering_.decimalLowerer().decimalMeta(v)})
            : v;
    if (argType->isOptional()) {
      number = lowering_.builder_.CreateSelect(
          lowering_.builder_.CreateExtractValue(value.value, 1), number, nan);
    }
    lowering_.refCounts().dropOperand(value);
    return number;
  }
  case Builtin::ParseInt:
  case Builtin::ParseFloat: {
    bool isInt = call->builtin == Builtin::ParseInt;
    Lowering::Operand str = lowering_.refCounts().emitOperand(
        call->args[0], call->args.size() == 1 || AssignLowerer::cannotAssign(call->args[1]));
    llvm::Value *result;
    if (isInt) {
      llvm::Value *radix = call->args.size() > 1 ? lowering_.exprLowerer().emitExpr(call->args[1])
                                                 : lowering_.builder_.getInt32(0);
      result = lowering_.builder_.CreateCall(
          lowering_.rt_.parseInt, {lowering_.stringLowerer().stringObj(str.value),
                                   lowering_.stringLowerer().stringMeta(str.value), radix});
    } else {
      result = lowering_.builder_.CreateCall(lowering_.rt_.parseFloat,
                                             {lowering_.stringLowerer().stringObj(str.value),
                                              lowering_.stringLowerer().stringMeta(str.value)});
    }
    lowering_.refCounts().dropOperand(str);
    return call->type->isInteger() ? emitCheckedInteger(result, call->type) : result;
  }
  case Builtin::IsNaN:
  case Builtin::IsFinite:
  case Builtin::IsInteger:
  case Builtin::IsSafeInteger: {
    llvm::Value *x = lowering_.exprLowerer().emitExpr(call->args[0]);
    if (argType->isInt()) {
      return call->builtin == Builtin::IsNaN ? lowering_.builder_.getFalse()
                                             : lowering_.builder_.getTrue();
    }
    if (argType->isInteger()) {
      if (call->builtin == Builtin::IsNaN) {
        return lowering_.builder_.getFalse();
      }
      if (call->builtin != Builtin::IsSafeInteger) {
        return lowering_.builder_.getTrue();
      }
      llvm::Value *limit = lowering_.builder_.getInt64((int64_t(1) << 53) - 1);
      return lowering_.builder_.CreateICmpULE(lowering_.builder_.CreateAdd(x, limit),
                                              lowering_.builder_.CreateAdd(limit, limit));
    }
    if (call->builtin == Builtin::IsNaN) {
      return lowering_.builder_.CreateFCmpUNO(x, x);
    }
    llvm::Value *magnitude = lowering_.builder_.CreateUnaryIntrinsic(llvm::Intrinsic::fabs, x);
    llvm::Value *finite =
        lowering_.builder_.CreateFCmpOLT(magnitude, llvm::ConstantFP::getInfinity(f64));
    if (call->builtin == Builtin::IsFinite) {
      return finite;
    }
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
      values.push_back(lowering_.exprLowerer().emitExpr(arg));
    }
    llvm::Value *array =
        lowering_.mathLowerer().spillArray(values, units ? lowering_.builder_.getInt32Ty() : f64);
    llvm::Value *text = lowering_.stringLowerer().callReturningString(
        units ? lowering_.rt_.strFromCharCodes : lowering_.rt_.strFromCodePoints,
        {array, lowering_.builder_.getInt64(values.size())});
    lowering_.emitThrowCheckIf(!units);
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
    llvm::Value *result = lowering_.stringLowerer().callReturningString(
        lowering_.processLowerer().processFunction(name, lowering_.builder_.getVoidTy(),
                                                   {ptr, ptr, lowering_.builder_.getInt64Ty()}),
        {lowering_.stringLowerer().stringObj(text.value),
         lowering_.stringLowerer().stringMeta(text.value)});
    lowering_.refCounts().dropOperand(text);
    lowering_.throwLowerer().emitThrowCheck();
    return result;
  }
  case Builtin::NumToFixed: {
    llvm::Value *x = lowering_.exprLowerer().emitExpr(cast<MemberExpr>(call->callee)->object);
    llvm::Value *digits =
        hasArg ? lowering_.exprLowerer().emitExpr(call->args[0]) : lowering_.builder_.getInt64(0);
    llvm::Value *text =
        lowering_.stringLowerer().callReturningString(lowering_.rt_.numToFixed, {x, digits});
    lowering_.emitThrowCheckIf(hasArg);
    return text;
  }
  case Builtin::NumToRadix: {
    llvm::Value *x = lowering_.exprLowerer().emitExpr(cast<MemberExpr>(call->callee)->object);
    llvm::Value *radix = lowering_.exprLowerer().emitExpr(call->args[0]);
    llvm::Value *text =
        lowering_.stringLowerer().callReturningString(lowering_.rt_.numToRadix, {x, radix});
    lowering_.throwLowerer().emitThrowCheck();
    return text;
  }
  default:
    llvm_unreachable("not a function of Number");
  }
}

}
