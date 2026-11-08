#include "compiler/lower/values/number_lowerer/number_lowerer.h"
#include "compiler/lower/expr/assign_lowerer/assign_lowerer.h"
#include "compiler/lower/expr/expr_lowerer/expr_lowerer.h"
#include "compiler/lower/function_frame/function_frame.h"
#include "compiler/lower/library/decimal/decimal_lowerer/decimal_lowerer.h"
#include "compiler/lower/library/math/math_lowerer/math_lowerer.h"
#include "compiler/lower/library/process/process_lowerer/process_lowerer.h"
#include "compiler/lower/library/result/result_lowerer/result_lowerer.h"
#include "compiler/lower/memory/ref_counts/ref_counts.h"
#include "compiler/lower/module_builder/module_builder.h"
#include "compiler/lower/stmt/stmt_lowerer/stmt_lowerer.h"
#include "compiler/lower/values/string_lowerer/string_lowerer.h"
#include "llvm/IR/Intrinsics.h"
#include "llvm/IR/MDBuilder.h"
#include "llvm/Support/ErrorHandling.h"

using llvm::cast;

namespace tz {

NumberLowerer::NumberLowerer(Lowering &owner)
    : lowering(owner) {}

llvm::Value *NumberLowerer::toI64(llvm::Value *value, const Type *type) {
  llvm::Type *i64 = this->lowering.builder.getInt64Ty();

  return type->isUnsigned() ? this->lowering.builder.CreateZExtOrTrunc(value, i64)
                            : this->lowering.builder.CreateSExtOrTrunc(value, i64);
}

llvm::Value *NumberLowerer::emitCheckedIntCast(llvm::Value *value, const Type *from,
                                               const Type *to) {
  if (to->holds(from)) {
    return this->intCast(value, from, to);
  }
  llvm::IRBuilder<> &b = this->lowering.builder;
  llvm::Type *i64 = b.getInt64Ty();
  llvm::Value *wide = this->toI64(value, from);
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
  llvm::BasicBlock *badBlock = this->lowering.functionFrame().newBlock("int.does.not.fit");
  llvm::BasicBlock *okBlock = this->lowering.functionFrame().newBlock("int.fits");
  b.CreateCondBr(fits, okBlock, badBlock,
                 llvm::MDBuilder(this->lowering.ctx).createLikelyBranchWeights());
  this->lowering.functionFrame().switchTo(badBlock);
  b.CreateCall(this->lowering.processLowerer().processFunction(
                   "tz_panic_int_does_not_fit", b.getVoidTy(), {i64, b.getInt32Ty(), b.getPtrTy()}),
               {wide, b.getInt32(from->isUnsigned() ? 1 : 0), b.CreateGlobalString(to->name())});
  b.CreateUnreachable();
  this->lowering.functionFrame().switchTo(okBlock);

  return this->intCast(value, from, to);
}

llvm::Value *NumberLowerer::intCast(llvm::Value *value, const Type *from, const Type *to) {
  llvm::Type *target = this->lowering.moduleBuilder().lower(to);

  return from->isUnsigned() ? this->lowering.builder.CreateZExtOrTrunc(value, target)
                            : this->lowering.builder.CreateSExtOrTrunc(value, target);
}

llvm::Value *NumberLowerer::toDouble(llvm::Value *value, const Type *from) {
  llvm::Type *f64 = this->lowering.builder.getDoubleTy();

  return from->isUnsigned() ? this->lowering.builder.CreateUIToFP(value, f64)
                            : this->lowering.builder.CreateSIToFP(value, f64);
}

llvm::Value *NumberLowerer::fromDouble(llvm::Value *value, const Type *to) {
  llvm::Type *target = this->lowering.moduleBuilder().lower(to);

  return to->isUnsigned() ? this->lowering.builder.CreateFPToUI(value, target)
                          : this->lowering.builder.CreateFPToSI(value, target);
}

llvm::Value *NumberLowerer::emitCheckedInteger(llvm::Value *value, const Type *to) {
  llvm::Type *f64 = this->lowering.builder.getDoubleTy();
  double lowest = double(to->intMin());
  double above = to->bitWidth() == 64
                     ? (to->isUnsigned() ? 18446744073709551616.0 : 9223372036854775808.0)
                     : double(to->intMax()) + 1;
  llvm::Value *low = llvm::ConstantFP::get(f64, lowest);
  llvm::Value *high = llvm::ConstantFP::get(f64, above);
  llvm::Value *fits =
      this->lowering.builder.CreateAnd(this->lowering.builder.CreateFCmpOGE(value, low),
                                       this->lowering.builder.CreateFCmpOLT(value, high));
  llvm::BasicBlock *badBlock = this->lowering.functionFrame().newBlock("int.does.not.fit");
  llvm::BasicBlock *okBlock = this->lowering.functionFrame().newBlock("int.fits");
  this->lowering.builder.CreateCondBr(
      fits, okBlock, badBlock, llvm::MDBuilder(this->lowering.ctx).createLikelyBranchWeights());
  this->lowering.functionFrame().switchTo(badBlock);
  this->lowering.builder.CreateCall(this->lowering.processLowerer().processFunction(
                                        "tz_panic_does_not_fit", this->lowering.builder.getVoidTy(),
                                        {f64, this->lowering.builder.getPtrTy()}),
                                    {value, this->lowering.builder.CreateGlobalString(to->name())});
  this->lowering.builder.CreateUnreachable();
  this->lowering.functionFrame().switchTo(okBlock);

  return this->fromDouble(value, to);
}

llvm::Value *NumberLowerer::emitStringToNumber(llvm::Value *str) {
  return this->lowering.builder.CreateCall(this->lowering.rt_.strToNumber,
                                           {this->lowering.stringLowerer().stringObj(str),
                                            this->lowering.stringLowerer().stringMeta(str)});
}

llvm::Value *NumberLowerer::emitNumberBuiltin(CallExpr *call) {
  llvm::Type *f64 = this->lowering.builder.getDoubleTy();
  bool hasArg = !call->args.empty();
  const Type *argType = hasArg ? call->args[0]->type : nullptr;

  switch (call->builtin()) {
    case Builtin::NumberOf: {
      if (!hasArg) {
        return llvm::ConstantFP::get(f64, 0.0);
      }
      Lowering::Operand value = this->lowering.refCounts().emitOperand(call->args[0], true);
      const Type *base = argType;
      llvm::Value *v = value.value;
      llvm::Type *i64 = this->lowering.builder.getInt64Ty();
      if (base->isString() && call->type->isResult()) {
        llvm::Value *parsed = this->lowering.builder.CreateCall(
            this->lowering.processLowerer().processFunction(
                "tz_str_to_number_r", f64,
                {this->lowering.builder.getPtrTy(), i64, this->lowering.builder.getPtrTy()}),
            {this->lowering.stringLowerer().stringObj(v),
             this->lowering.stringLowerer().stringMeta(v),
             this->lowering.resultLowerer().failureMaker(call->type->errType())});
        this->lowering.refCounts().dropOperand(value);
        return this->lowering.resultLowerer().catchFailure(parsed, call->type, call->loc());
      }
      llvm::Value *number =
          base->isString() ? this->emitStringToNumber(v)
          : base->isBool() ? this->lowering.builder.CreateUIToFP(v, f64)
          : base->isDecimal()
              ? this->lowering.builder.CreateCall(this->lowering.decimalLowerer().decimalFunction(
                                                      "tz_dec_to_f64", f64, {i64, i64}),
                                                  {this->lowering.decimalLowerer().decimalCoef(v),
                                                   this->lowering.decimalLowerer().decimalMeta(v)})
          : base->isInteger() ? this->toDouble(v, base)
                              : v;
      this->lowering.refCounts().dropOperand(value);
      return number;
    }
    case Builtin::ParseInt:
    case Builtin::ParseFloat: {
      bool isInt = call->builtin() == Builtin::ParseInt;
      Lowering::Operand str = this->lowering.refCounts().emitOperand(
          call->args[0], call->args.size() == 1 || AssignLowerer::cannotAssign(call->args[1]));
      llvm::Type *ptr = this->lowering.builder.getPtrTy();
      llvm::Type *i64 = this->lowering.builder.getInt64Ty();
      llvm::Value *maker = this->lowering.resultLowerer().failureMaker(call->type->errType());
      llvm::Value *result;
      if (isInt) {
        llvm::Value *radix = call->args.size() > 1
                                 ? this->lowering.exprLowerer().emitExpr(call->args[1])
                                 : this->lowering.builder.getInt64(0);
        result = this->lowering.builder.CreateCall(
            this->lowering.processLowerer().processFunction("tz_parse_int_r", i64,
                                                            {ptr, i64, i64, ptr}),
            {this->lowering.stringLowerer().stringObj(str.value),
             this->lowering.stringLowerer().stringMeta(str.value), radix, maker});
      } else {
        result = this->lowering.builder.CreateCall(
            this->lowering.processLowerer().processFunction("tz_parse_float_r", f64,
                                                            {ptr, i64, ptr}),
            {this->lowering.stringLowerer().stringObj(str.value),
             this->lowering.stringLowerer().stringMeta(str.value), maker});
      }
      this->lowering.refCounts().dropOperand(str);
      return this->lowering.resultLowerer().catchFailure(result, call->type, call->loc());
    }
    case Builtin::IsInteger:
    case Builtin::IsSafeInteger: {
      llvm::Value *x = this->lowering.exprLowerer().emitExpr(call->args[0]);
      if (argType->isInteger() && argType->bitWidth() < 64) {
        return this->lowering.builder.getTrue();
      }
      if (argType->isInteger()) {
        if (call->builtin() != Builtin::IsSafeInteger) {
          return this->lowering.builder.getTrue();
        }
        llvm::Value *limit = this->lowering.builder.getInt64((int64_t(1) << 53) - 1);
        if (argType->isUnsigned()) {
          return this->lowering.builder.CreateICmpULE(x, limit);
        }
        return this->lowering.builder.CreateICmpULE(this->lowering.builder.CreateAdd(x, limit),
                                                    this->lowering.builder.CreateAdd(limit, limit));
      }
      llvm::Value *magnitude =
          this->lowering.builder.CreateUnaryIntrinsic(llvm::Intrinsic::fabs, x);
      llvm::Value *finite =
          this->lowering.builder.CreateFCmpOLT(magnitude, llvm::ConstantFP::getInfinity(f64));
      llvm::Value *whole = this->lowering.builder.CreateFCmpOEQ(
          this->lowering.builder.CreateUnaryIntrinsic(llvm::Intrinsic::trunc, x), x);
      llvm::Value *integer = this->lowering.builder.CreateAnd(finite, whole);
      if (call->builtin() == Builtin::IsInteger) {
        return integer;
      }
      llvm::Value *safe = this->lowering.builder.CreateFCmpOLE(
          magnitude, llvm::ConstantFP::get(f64, 9007199254740991.0));
      return this->lowering.builder.CreateAnd(integer, safe);
    }
    case Builtin::FromCharCode:
    case Builtin::FromCodePoint: {
      if (!hasArg) {
        return this->lowering.stringLowerer().stringLiteral(u"");
      }
      bool units = call->builtin() == Builtin::FromCharCode;
      std::vector<llvm::Value *> values;
      for (Expr *arg : call->args) {
        llvm::Value *value = this->lowering.exprLowerer().emitExpr(arg);
        values.push_back(
            units ? this->lowering.builder.CreateTrunc(value, this->lowering.builder.getInt32Ty())
                  : value);
      }
      llvm::Value *array = this->lowering.mathLowerer().spillArray(
          values, units ? this->lowering.builder.getInt32Ty() : f64);
      llvm::Value *text = this->lowering.stringLowerer().callReturningString(
          units ? this->lowering.rt_.strFromCharCodes : this->lowering.rt_.strFromCodePoints,
          {array, this->lowering.builder.getInt64(values.size())});
      return text;
    }
    case Builtin::UriEncodeComponent:
    case Builtin::UriEncode:
    case Builtin::UriDecodeComponent:
    case Builtin::UriDecode: {
      llvm::StringRef name =
          call->builtin() == Builtin::UriEncodeComponent   ? "tz_uri_encode_component"
          : call->builtin() == Builtin::UriEncode          ? "tz_uri_encode"
          : call->builtin() == Builtin::UriDecodeComponent ? "tz_uri_decode_component"
                                                           : "tz_uri_decode";
      llvm::Type *ptr = this->lowering.builder.getPtrTy();
      Lowering::Operand text = this->lowering.refCounts().emitOperand(call->args[0], true);
      if (call->type->isResult()) {
        llvm::Value *result = this->lowering.stringLowerer().callReturningString(
            this->lowering.processLowerer().processFunction(
                name, this->lowering.builder.getVoidTy(),
                {ptr, ptr, this->lowering.builder.getInt64Ty(), ptr}),
            {this->lowering.stringLowerer().stringObj(text.value),
             this->lowering.stringLowerer().stringMeta(text.value),
             this->lowering.resultLowerer().failureMaker(call->type->errType())});
        this->lowering.refCounts().dropOperand(text);
        return this->lowering.resultLowerer().catchFailure(result, call->type, call->loc());
      }
      llvm::Value *result = this->lowering.stringLowerer().callReturningString(
          this->lowering.processLowerer().processFunction(
              name, this->lowering.builder.getVoidTy(),
              {ptr, ptr, this->lowering.builder.getInt64Ty()}),
          {this->lowering.stringLowerer().stringObj(text.value),
           this->lowering.stringLowerer().stringMeta(text.value)});
      this->lowering.refCounts().dropOperand(text);
      return result;
    }
    case Builtin::NumToFixed: {
      llvm::Value *x =
          this->lowering.exprLowerer().emitExpr(cast<MemberExpr>(call->callee)->object);
      llvm::Value *digits = hasArg ? this->lowering.exprLowerer().emitExpr(call->args[0])
                                   : this->lowering.builder.getInt64(0);
      llvm::Value *text = this->lowering.stringLowerer().callReturningString(
          this->lowering.rt_.numToFixed, {x, digits});
      return text;
    }
    case Builtin::NumToRadix: {
      llvm::Value *x =
          this->lowering.exprLowerer().emitExpr(cast<MemberExpr>(call->callee)->object);
      llvm::Value *radix = this->lowering.exprLowerer().emitExpr(call->args[0]);
      llvm::Value *text = this->lowering.stringLowerer().callReturningString(
          this->lowering.rt_.numToRadix, {x, radix});
      return text;
    }
    case Builtin::NumConvert: {
      Expr *object = cast<MemberExpr>(call->callee)->object;
      llvm::Value *x = this->lowering.exprLowerer().emitExpr(object);
      const Type *from = object->type;
      const Type *to = call->type;
      if (from == to) {
        return x;
      }
      if (to->isDecimal()) {
        return this->lowering.decimalLowerer().decimalFromInteger(x, from);
      }
      if (to->isFloat()) {
        return this->toDouble(x, from);
      }
      if (from->isFloat()) {
        return call->rangeProven ? this->fromDouble(x, to) : this->emitCheckedInteger(x, to);
      }
      return call->rangeProven ? this->intCast(x, from, to) : this->emitCheckedIntCast(x, from, to);
    }
    default:
      llvm_unreachable("not a function of Number");
  }
}

}
