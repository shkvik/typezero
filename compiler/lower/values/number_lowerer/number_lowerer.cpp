#include "compiler/lower/values/number_lowerer/number_lowerer.h"
#include "compiler/lower/module_builder/module_builder.h"
#include "compiler/lower/function_frame/function_frame.h"
#include "compiler/lower/memory/ref_counts/ref_counts.h"
#include "compiler/lower/stmt/stmt_lowerer/stmt_lowerer.h"
#include "compiler/lower/expr/expr_lowerer/expr_lowerer.h"
#include "compiler/lower/expr/assign_lowerer/assign_lowerer.h"
#include "compiler/lower/values/string_lowerer/string_lowerer.h"
#include "compiler/library/math/math_lowerer/math_lowerer.h"
#include "compiler/lower/errors/throw_lowerer/throw_lowerer.h"
#include "compiler/library/decimal/decimal_lowerer/decimal_lowerer.h"
#include "compiler/library/process/process_lowerer/process_lowerer.h"
#include "llvm/IR/Intrinsics.h"
#include "llvm/IR/MDBuilder.h"
#include "llvm/Support/ErrorHandling.h"

using llvm::cast;
using llvm::dyn_cast;
using llvm::isa;
using llvm::BasicBlock;
using llvm::Value;

namespace tz {

NumberLowerer::NumberLowerer(CodeGen &codegen) : codegen_(codegen) {}

// RangeError if the double does not fit (NaN does not either).
llvm::Value *NumberLowerer::emitCheckedInteger(llvm::Value *value, const Type *to) {
  bool isInt = to->isInt();
  llvm::Type *f64 = codegen_.builder_.getDoubleTy();
  llvm::Value *low = llvm::ConstantFP::get(f64, isInt ? -2147483648.0 : -9223372036854775808.0);
  llvm::Value *high = llvm::ConstantFP::get(f64, isInt ? 2147483648.0 : 9223372036854775808.0);
  llvm::Value *fits =
      codegen_.builder_.CreateAnd(codegen_.builder_.CreateFCmpOGE(value, low), codegen_.builder_.CreateFCmpOLT(value, high));
  llvm::BasicBlock *badBlock = codegen_.functionFrame().newBlock("int.does.not.fit");
  llvm::BasicBlock *okBlock = codegen_.functionFrame().newBlock("int.fits");
  codegen_.builder_.CreateCondBr(fits, okBlock, badBlock, llvm::MDBuilder(codegen_.ctx_).createLikelyBranchWeights());
  codegen_.functionFrame().switchTo(badBlock);
  codegen_.builder_.CreateCall(
      codegen_.processLowerer().processFunction("tz_throw_does_not_fit", codegen_.builder_.getVoidTy(), {f64, codegen_.builder_.getInt32Ty()}),
      {value, codegen_.builder_.getInt32(isInt ? 32 : 64)});
  codegen_.throwLowerer().emitRuntimeThrow();
  codegen_.functionFrame().switchTo(okBlock);

  return codegen_.builder_.CreateFPToSI(value, codegen_.moduleBuilder().lower(to));
}

llvm::Value *NumberLowerer::emitStringToNumber(llvm::Value *str) {
  return codegen_.builder_.CreateCall(codegen_.rt_.strToNumber, {codegen_.stringLowerer().stringObj(str), codegen_.stringLowerer().stringMeta(str)});
}

llvm::Value *NumberLowerer::emitNumberBuiltin(CallExpr *call) {
  llvm::Type *f64 = codegen_.builder_.getDoubleTy();
  llvm::Value *nan = llvm::ConstantFP::getNaN(f64);
  bool hasArg = !call->args.empty();
  const Type *argType = hasArg ? call->args[0]->type : nullptr;

  switch (call->builtin) {
  case Builtin::NumberOf: {
    if (!hasArg) {
      return llvm::ConstantFP::get(f64, 0.0);
    }
    if (argType->isUndefined()) {
      codegen_.stmtLowerer().emitDiscarded(call->args[0]);
      return nan;
    }
    CodeGen::Operand value = codegen_.refCounts().emitOperand(call->args[0], /*canBorrow=*/true);
    const Type *base = argType->nonOptional();
    llvm::Value *v =
        argType->isOptional() ? codegen_.builder_.CreateExtractValue(value.value, 0) : value.value;
    llvm::Type *i64 = codegen_.builder_.getInt64Ty();
    llvm::Value *number =
        base->isString()    ? emitStringToNumber(v)
        : base->isBool()    ? codegen_.builder_.CreateUIToFP(v, f64)
        : base->isDecimal() ? codegen_.builder_.CreateCall(codegen_.decimalLowerer().decimalFunction("tz_dec_to_f64", f64, {i64, i64}),
                                                  {codegen_.decimalLowerer().decimalCoef(v), codegen_.decimalLowerer().decimalMeta(v)})
                            : v; // Sema made numbers doubles
    if (argType->isOptional()) {
      number = codegen_.builder_.CreateSelect(codegen_.builder_.CreateExtractValue(value.value, 1), number, nan);
    }
    codegen_.refCounts().dropOperand(value);
    return number;
  }
  case Builtin::ParseInt:
  case Builtin::ParseFloat: {
    bool isInt = call->builtin == Builtin::ParseInt;
    CodeGen::Operand str = codegen_.refCounts().emitOperand(call->args[0], call->args.size() == 1 || AssignLowerer::cannotAssign(call->args[1]));
    llvm::Value *result;
    if (isInt) {
      // A radix of 0 means none, also when it is given.
      llvm::Value *radix = call->args.size() > 1 ? codegen_.exprLowerer().emitExpr(call->args[1]) : codegen_.builder_.getInt32(0);
      result =
          codegen_.builder_.CreateCall(codegen_.rt_.parseInt, {codegen_.stringLowerer().stringObj(str.value), codegen_.stringLowerer().stringMeta(str.value), radix});
    } else {
      result = codegen_.builder_.CreateCall(codegen_.rt_.parseFloat, {codegen_.stringLowerer().stringObj(str.value), codegen_.stringLowerer().stringMeta(str.value)});
    }
    codegen_.refCounts().dropOperand(str);
    // parseInt where an int is expected: the number must fit, NaN does not.
    return call->type->isInteger() ? emitCheckedInteger(result, call->type) : result;
  }
  case Builtin::IsNaN:
  case Builtin::IsFinite:
  case Builtin::IsInteger:
  case Builtin::IsSafeInteger: {
    llvm::Value *x = codegen_.exprLowerer().emitExpr(call->args[0]);
    if (argType->isInt()) {
      return call->builtin == Builtin::IsNaN ? codegen_.builder_.getFalse() : codegen_.builder_.getTrue();
    }
    if (argType->isInteger()) { // a long: only isSafeInteger can be false
      if (call->builtin == Builtin::IsNaN) {
        return codegen_.builder_.getFalse();
      }
      if (call->builtin != Builtin::IsSafeInteger) {
        return codegen_.builder_.getTrue();
      }
      llvm::Value *limit = codegen_.builder_.getInt64((int64_t(1) << 53) - 1);
      return codegen_.builder_.CreateICmpULE(codegen_.builder_.CreateAdd(x, limit), codegen_.builder_.CreateAdd(limit, limit));
    }
    if (call->builtin == Builtin::IsNaN) {
      return codegen_.builder_.CreateFCmpUNO(x, x);
    }
    llvm::Value *magnitude = codegen_.builder_.CreateUnaryIntrinsic(llvm::Intrinsic::fabs, x);
    llvm::Value *finite = codegen_.builder_.CreateFCmpOLT(magnitude, llvm::ConstantFP::getInfinity(f64));
    if (call->builtin == Builtin::IsFinite) {
      return finite;
    }
    llvm::Value *whole =
        codegen_.builder_.CreateFCmpOEQ(codegen_.builder_.CreateUnaryIntrinsic(llvm::Intrinsic::trunc, x), x);
    llvm::Value *integer = codegen_.builder_.CreateAnd(finite, whole);
    if (call->builtin == Builtin::IsInteger) {
      return integer;
    }
    llvm::Value *safe =
        codegen_.builder_.CreateFCmpOLE(magnitude, llvm::ConstantFP::get(f64, 9007199254740991.0));
    return codegen_.builder_.CreateAnd(integer, safe);
  }
  case Builtin::FromCharCode:
  case Builtin::FromCodePoint: {
    if (!hasArg) {
      return codegen_.stringLowerer().stringLiteral(u"");
    }
    bool units = call->builtin == Builtin::FromCharCode;
    std::vector<llvm::Value *> values;
    for (Expr *arg : call->args) {
      values.push_back(codegen_.exprLowerer().emitExpr(arg));
    }
    llvm::Value *array = codegen_.mathLowerer().spillArray(values, units ? codegen_.builder_.getInt32Ty() : f64);
    llvm::Value *text = codegen_.stringLowerer().callReturningString(units ? codegen_.rt_.strFromCharCodes : codegen_.rt_.strFromCodePoints,
                                            {array, codegen_.builder_.getInt64(values.size())});
    codegen_.emitThrowCheckIf(!units); // a code point out of range
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
    llvm::Type *ptr = codegen_.builder_.getPtrTy();
    CodeGen::Operand text = codegen_.refCounts().emitOperand(call->args[0], /*canBorrow=*/true);
    llvm::Value *result = codegen_.stringLowerer().callReturningString(
        codegen_.processLowerer().processFunction(name, codegen_.builder_.getVoidTy(), {ptr, ptr, codegen_.builder_.getInt64Ty()}),
        {codegen_.stringLowerer().stringObj(text.value), codegen_.stringLowerer().stringMeta(text.value)});
    codegen_.refCounts().dropOperand(text);
    codegen_.throwLowerer().emitThrowCheck(); // URIError: URI malformed
    return result;
  }
  case Builtin::NumToFixed: {
    llvm::Value *x = codegen_.exprLowerer().emitExpr(cast<MemberExpr>(call->callee)->object);
    llvm::Value *digits = hasArg ? codegen_.exprLowerer().emitExpr(call->args[0]) : codegen_.builder_.getInt64(0);
    llvm::Value *text = codegen_.stringLowerer().callReturningString(codegen_.rt_.numToFixed, {x, digits});
    codegen_.emitThrowCheckIf(hasArg); // digits out of range
    return text;
  }
  case Builtin::NumToRadix: {
    llvm::Value *x = codegen_.exprLowerer().emitExpr(cast<MemberExpr>(call->callee)->object);
    llvm::Value *radix = codegen_.exprLowerer().emitExpr(call->args[0]);
    llvm::Value *text = codegen_.stringLowerer().callReturningString(codegen_.rt_.numToRadix, {x, radix});
    codegen_.throwLowerer().emitThrowCheck(); // a radix out of range
    return text;
  }
  default:
    llvm_unreachable("not a function of Number");
  }
}

} // namespace tz
