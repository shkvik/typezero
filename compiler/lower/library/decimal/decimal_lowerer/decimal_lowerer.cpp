#include "compiler/lower/library/decimal/decimal_lowerer/decimal_lowerer.h"
#include "compiler/lower/expr/assign_lowerer/assign_lowerer.h"
#include "compiler/lower/expr/binary_lowerer/binary_lowerer.h"
#include "compiler/lower/expr/expr_lowerer/expr_lowerer.h"
#include "compiler/lower/function_frame/function_frame.h"
#include "compiler/lower/layouts/type_descriptors/type_descriptors.h"
#include "compiler/lower/library/result/result_lowerer/result_lowerer.h"
#include "compiler/lower/memory/ref_counts/ref_counts.h"
#include "compiler/lower/objects/object_maker/object_maker.h"
#include "compiler/lower/runtime_functions/runtime_functions.h"
#include "compiler/lower/types/optional_lowerer/optional_lowerer.h"
#include "compiler/lower/values/number_lowerer/number_lowerer.h"
#include "compiler/lower/values/string_lowerer/string_lowerer.h"
#include "compiler/program/library/decimal/decimal_literal/decimal_literal.h"
#include "llvm/ADT/STLExtras.h"

using llvm::cast;
using llvm::dyn_cast;

namespace tz {

namespace {

constexpr uint64_t kDecLong = uint64_t(1) << 62;

constexpr int64_t kDecScaleMax = 0x7fffffff;

constexpr int64_t kLiteralRefCount = int64_t(1) << 62;

constexpr int64_t kDivisionPlaces = 20;

enum Fold : int32_t { kFoldSum, kFoldMax, kFoldMin };

}

DecimalLowerer::DecimalLowerer(Lowering &owner)
    : lowering(owner) {}

llvm::FunctionCallee DecimalLowerer::decimalFunction(llvm::StringRef name, llvm::Type *ret,
                                                     llvm::ArrayRef<llvm::Type *> params) {
  llvm::FunctionCallee callee = this->lowering.module_->getOrInsertFunction(
      name, llvm::FunctionType::get(ret, params, false));
  cast<llvm::Function>(callee.getCallee())->addFnAttr(llvm::Attribute::NoUnwind);

  return callee;
}

llvm::Value *DecimalLowerer::decimalCoef(llvm::Value *value) {
  return this->lowering.builder.CreateExtractValue(value, 0);
}

llvm::Value *DecimalLowerer::decimalMeta(llvm::Value *value) {
  return this->lowering.builder.CreateExtractValue(value, 1);
}

llvm::Value *DecimalLowerer::callReturningDecimal(llvm::StringRef name,
                                                  llvm::ArrayRef<llvm::Value *> args) {
  std::vector<llvm::Type *> params{this->lowering.builder.getPtrTy()};
  for (llvm::Value *arg : args) {
    params.push_back(arg->getType());
  }
  llvm::AllocaInst *out =
      this->lowering.functionFrame().createStackSlot(this->lowering.decTy, "dec.out");
  std::vector<llvm::Value *> all{out};
  all.insert(all.end(), args.begin(), args.end());
  this->lowering.builder.CreateCall(
      this->decimalFunction(name, this->lowering.builder.getVoidTy(), params), all);

  return this->lowering.builder.CreateLoad(this->lowering.decTy, out);
}

llvm::Function *DecimalLowerer::decimalRefCount(bool retain) {
  llvm::Function *&fn = retain ? this->decRetainFn : this->decReleaseFn;
  if (fn) {
    return fn;
  }
  auto *type =
      llvm::FunctionType::get(this->lowering.builder.getVoidTy(), {this->lowering.decTy}, false);
  fn = llvm::Function::Create(type, llvm::GlobalValue::InternalLinkage,
                              retain ? "tz.dec.retain" : "tz.dec.release",
                              this->lowering.module_.get());
  fn->addFnAttr(llvm::Attribute::AlwaysInline);
  fn->addFnAttr(llvm::Attribute::NoUnwind);
  llvm::IRBuilder<> b(llvm::BasicBlock::Create(this->lowering.ctx, "entry", fn));
  auto *countedBlock = llvm::BasicBlock::Create(this->lowering.ctx, "counted", fn);
  auto *doneBlock = llvm::BasicBlock::Create(this->lowering.ctx, "done", fn);
  llvm::Value *value = fn->getArg(0);
  llvm::Value *counted = b.CreateICmpSLT(b.CreateExtractValue(value, 1, "meta"), b.getInt64(0));
  b.CreateCondBr(counted, countedBlock, doneBlock);
  b.SetInsertPoint(countedBlock);
  llvm::Value *obj = b.CreateIntToPtr(b.CreateExtractValue(value, 0, "coef"), b.getPtrTy(), "obj");
  llvm::Value *count = b.CreateLoad(b.getInt64Ty(), obj, "rc");
  auto *freeBlock = retain ? nullptr : llvm::BasicBlock::Create(this->lowering.ctx, "free", fn);
  if (this->lowering.parallel_) {
    this->lowering.runtimeFunctions().countShared(b, obj, count, retain, freeBlock, doneBlock);
  }
  if (retain) {
    b.CreateStore(b.CreateAdd(count, b.getInt64(1)), obj);
    b.CreateBr(doneBlock);
  } else {
    llvm::Value *left = b.CreateSub(count, b.getInt64(1));
    b.CreateStore(left, obj);
    b.CreateCondBr(b.CreateICmpEQ(left, b.getInt64(0)), freeBlock, doneBlock);
    b.SetInsertPoint(freeBlock);
    b.CreateCall(this->decimalFunction("tz_dec_free", b.getVoidTy(), {b.getPtrTy()}), {obj});
    b.CreateBr(doneBlock);
  }
  b.SetInsertPoint(doneBlock);
  b.CreateRetVoid();

  return fn;
}

llvm::Constant *DecimalLowerer::emitDecimalLiteral(const Expr *expr) {
  bool negative = false;
  if (auto *unary = dyn_cast<UnaryExpr>(expr); unary && unary->op == UnaryOp::Neg) {
    negative = true;
    expr = unary->operand;
  }
  DecimalLiteral value;
  if (auto *lit = dyn_cast<IntLitExpr>(expr)) {
    value = lit->text.empty() ? DecimalLiteral::of(lit->value)
                              : DecimalLiteral::read(lit->text).value_or(value);
  } else {
    value = DecimalLiteral::read(cast<FloatLitExpr>(expr)->text).value_or(value);
  }
  llvm::Type *i64 = this->lowering.builder.getInt64Ty();
  auto shortValue = [&](uint64_t magnitude, int64_t scale) {
    uint64_t coef = negative ? 0 - magnitude : magnitude;

    return llvm::ConstantStruct::get(
        this->lowering.decTy,
        {llvm::ConstantInt::get(i64, coef), llvm::ConstantInt::get(i64, uint64_t(scale))});
  };
  if (value.isZero() || value.top() < kDecimalMinExp) {
    return shortValue(0, 0);
  }
  const std::string &digits = value.digits;
  int64_t exponent = value.exponent;
  if (digits.size() <= 19) {
    uint64_t magnitude = std::stoull(digits);
    uint64_t limit = negative ? uint64_t(1) << 63 : (uint64_t(1) << 63) - 1;
    if (exponent <= 0 && -exponent <= kDecScaleMax && magnitude <= limit) {
      return shortValue(magnitude, -exponent);
    }
    uint64_t power = 1;
    bool fits = exponent > 0 && exponent < 20;
    for (int64_t i = 0; fits && i < exponent; ++i) {
      power *= 10;
    }
    if (fits && magnitude <= limit / power) {
      return shortValue(magnitude * power, 0);
    }
  }
  std::vector<uint32_t> limbs;
  for (size_t end = digits.size(); end > 0;) {
    size_t start = end >= 9 ? end - 9 : 0;
    limbs.push_back(static_cast<uint32_t>(std::stoul(digits.substr(start, end - start))));
    end = start;
  }
  llvm::Type *i32 = this->lowering.builder.getInt32Ty();
  llvm::Constant *data = llvm::ConstantDataArray::get(this->lowering.ctx, limbs);
  auto *layout = llvm::StructType::get(this->lowering.ctx, {i64, i64, i32, i32, data->getType()});
  llvm::Constant *init = llvm::ConstantStruct::get(
      layout,
      {this->lowering.builder.getInt64(kLiteralRefCount),
       llvm::ConstantInt::get(i64, uint64_t(exponent)), this->lowering.builder.getInt32(negative),
       this->lowering.builder.getInt32(static_cast<uint32_t>(limbs.size())), data});
  auto *global = new llvm::GlobalVariable(*this->lowering.module_, layout, true,
                                          llvm::GlobalValue::PrivateLinkage, init, ".decimal");
  global->setUnnamedAddr(llvm::GlobalValue::UnnamedAddr::Global);
  global->setAlignment(llvm::Align(8));

  return llvm::ConstantStruct::get(
      this->lowering.decTy,
      {llvm::ConstantExpr::getPtrToInt(global, i64), this->lowering.builder.getInt64(kDecLong)});
}

llvm::Value *DecimalLowerer::decimalFromInteger(llvm::Value *value, const Type *from) {
  if (from->isUnsigned() && from->bitWidth() == 64) {
    return this->callReturningDecimal("tz_dec_from_u64", {value});
  }
  llvm::Value *coef = this->lowering.numberLowerer().toI64(value, from);
  llvm::Value *result = this->lowering.builder.CreateInsertValue(
      llvm::UndefValue::get(this->lowering.decTy), coef, 0);

  return this->lowering.builder.CreateInsertValue(result, this->lowering.builder.getInt64(0), 1);
}

llvm::Value *DecimalLowerer::emitDecimalArithmetic(BinaryOp op, llvm::Value *lhs,
                                                   llvm::Value *rhs) {
  llvm::Value *a = this->decimalCoef(lhs);
  llvm::Value *am = this->decimalMeta(lhs);
  if (op == BinaryOp::Pow) {
    return this->callReturningDecimal("tz_dec_pow", {a, am, rhs});
  }
  llvm::Value *b = this->decimalCoef(rhs);
  llvm::Value *bm = this->decimalMeta(rhs);
  switch (op) {
  case BinaryOp::Add:
    return this->callReturningDecimal("tz_dec_add", {a, am, b, bm});
  case BinaryOp::Sub:
    return this->callReturningDecimal("tz_dec_sub", {a, am, b, bm});
  case BinaryOp::Mul:
    return this->callReturningDecimal("tz_dec_mul", {a, am, b, bm});
  case BinaryOp::Div:
    return this->callReturningDecimal(
        "tz_dec_div", {a, am, b, bm, this->lowering.builder.getInt64(kDivisionPlaces),
                       this->lowering.builder.getInt32(kRoundHalfUp)});
  case BinaryOp::Rem:
    return this->callReturningDecimal("tz_dec_rem", {a, am, b, bm});
  default:
    llvm_unreachable("not an operator of decimals");
  }
}

llvm::Value *DecimalLowerer::emitDecimalBinary(BinaryExpr *bin) {
  Lowering::Operand a =
      this->lowering.refCounts().emitOperand(bin->lhs, AssignLowerer::cannotAssign(bin->rhs));
  Lowering::Operand b = this->lowering.refCounts().emitOperand(bin->rhs, true);
  bool arithmetic = false;
  llvm::Value *result;
  switch (bin->op) {
  case BinaryOp::Lt:
  case BinaryOp::Le:
  case BinaryOp::Gt:
  case BinaryOp::Ge: {
    llvm::Type *i64 = this->lowering.builder.getInt64Ty();
    llvm::Value *order = this->lowering.builder.CreateCall(
        this->decimalFunction("tz_dec_cmp", this->lowering.builder.getInt32Ty(),
                              {i64, i64, i64, i64}),
        {this->decimalCoef(a.value), this->decimalMeta(a.value), this->decimalCoef(b.value),
         this->decimalMeta(b.value)});
    result = this->lowering.binaryLowerer().emitComparison(
        bin->op, order, this->lowering.builder.getInt32(0), Type::intTy());
    break;
  }
  default:
    result = this->emitDecimalArithmetic(bin->op, a.value, b.value);
    arithmetic = true;
    break;
  }
  this->lowering.refCounts().dropOperand(a);
  this->lowering.refCounts().dropOperand(b);

  return result;
}

llvm::Value *DecimalLowerer::emitDecimalCompound(BinaryOp op, llvm::Value *old, Expr *value) {
  Lowering::Operand rhs = this->lowering.refCounts().emitOperand(value, true);
  llvm::Value *result = this->emitDecimalArithmetic(op, old, rhs.value);
  this->lowering.refCounts().dropOperand(rhs);

  return result;
}

llvm::Value *DecimalLowerer::emitDecimalUpdate(UpdateExpr *update, llvm::Value *address) {
  llvm::Value *old = this->lowering.builder.CreateLoad(this->lowering.decTy, address);
  llvm::Constant *one =
      llvm::ConstantStruct::get(this->lowering.decTy, {this->lowering.builder.getInt64(1),
                                                       this->lowering.builder.getInt64(0)});
  llvm::Value *updated =
      this->emitDecimalArithmetic(update->increment ? BinaryOp::Add : BinaryOp::Sub, old, one);
  this->lowering.builder.CreateStore(updated, address);
  if (!update->prefix) {
    return old;
  }
  this->lowering.refCounts().releaseValue(old, Type::decimalTy());
  this->lowering.refCounts().retainValue(updated, Type::decimalTy());

  return updated;
}

llvm::Value *DecimalLowerer::decimalSign(llvm::Value *value) {
  llvm::Type *i64 = this->lowering.builder.getInt64Ty();

  return this->lowering.builder.CreateCall(
      this->decimalFunction("tz_dec_sign", this->lowering.builder.getInt32Ty(), {i64, i64}),
      {this->decimalCoef(value), this->decimalMeta(value)});
}

llvm::Value *DecimalLowerer::emitRoundingMode(CallExpr *call, size_t index) {
  if (index >= call->args.size()) {
    return this->lowering.builder.getInt32(kRoundHalfUp);
  }
  Expr *arg = call->args[index];
  if (auto *lit = dyn_cast<StringLitExpr>(arg)) {
    return this->lowering.builder.getInt32(
        static_cast<uint32_t>(DecimalLiteral::roundingMode(lit->value)));
  }
  Lowering::Operand mode = this->lowering.refCounts().emitOperand(arg, true);
  llvm::Value *result = this->lowering.builder.CreateCall(
      this->decimalFunction(
          "tz_dec_rounding_mode", this->lowering.builder.getInt32Ty(),
          {this->lowering.builder.getPtrTy(), this->lowering.builder.getInt64Ty()}),
      {this->lowering.stringLowerer().stringObj(mode.value),
       this->lowering.stringLowerer().stringMeta(mode.value)});
  this->lowering.refCounts().dropOperand(mode);

  return result;
}

llvm::Value *DecimalLowerer::emitDecimalFold(CallExpr *call) {
  int32_t op = call->builtin() == Builtin::DecSum   ? kFoldSum
               : call->builtin() == Builtin::DecMax ? kFoldMax
                                                    : kFoldMin;
  std::vector<llvm::Value *> values = this->lowering.exprLowerer().emitExprs(call->args);
  std::vector<Lowering::Operand> args;
  for (size_t i = 0; i < values.size(); ++i) {
    args.push_back({values[i], true, call->args[i]->type});
  }
  llvm::Type *i32 = this->lowering.builder.getInt32Ty();
  llvm::Type *ptr = this->lowering.builder.getPtrTy();
  llvm::Value *acc =
      this->lowering.functionFrame().createStackSlot(this->lowering.decTy, "fold.acc");
  this->lowering.builder.CreateStore(llvm::Constant::getNullValue(this->lowering.decTy), acc);
  llvm::Value *has = this->lowering.functionFrame().createStackSlot(i32, "fold.has");
  this->lowering.builder.CreateStore(this->lowering.builder.getInt32(op == kFoldSum), has);
  for (size_t i = 0; i < args.size(); ++i) {
    if (call->spreads(i)) {
      this->lowering.builder.CreateCall(
          this->decimalFunction("tz_dec_fold_array", this->lowering.builder.getVoidTy(),
                                {ptr, ptr, ptr, i32}),
          {acc, has, args[i].value, this->lowering.builder.getInt32(op)});
    } else {
      this->lowering.builder.CreateCall(
          this->decimalFunction("tz_dec_fold", this->lowering.builder.getVoidTy(),
                                {ptr, ptr, ptr, this->lowering.builder.getInt64Ty(), i32}),
          {acc, has, this->lowering.functionFrame().spill(args[i].value, "fold.value"),
           this->lowering.builder.getInt64(1), this->lowering.builder.getInt32(op)});
    }
  }
  for (const Lowering::Operand &arg : args) {
    this->lowering.refCounts().dropOperand(arg);
  }
  llvm::Value *result = this->lowering.builder.CreateLoad(this->lowering.decTy, acc);
  if (!call->type->isOptional()) {
    return result;
  }

  return this->lowering.optionalLowerer().makeOptional(
      result,
      this->lowering.builder.CreateICmpNE(this->lowering.builder.CreateLoad(i32, has),
                                          this->lowering.builder.getInt32(0)),
      call->type);
}

llvm::Value *DecimalLowerer::emitDecimalCall(CallExpr *call) {
  llvm::Type *i32 = this->lowering.builder.getInt32Ty(), *i64 = this->lowering.builder.getInt64Ty(),
             *ptr = this->lowering.builder.getPtrTy();
  llvm::Type *voidTy = this->lowering.builder.getVoidTy();
  switch (call->builtin()) {
    case Builtin::DecimalOf: {
      Expr *arg = call->args[0];
      if (arg->type->isDecimal()) {
        return this->lowering.exprLowerer().emitExpr(arg);
      }
      if (arg->type->isFloat()) {
        llvm::Value *result = this->callReturningDecimal(
            "tz_dec_from_f64", {this->lowering.exprLowerer().emitExpr(arg)});
        return result;
      }
      static const std::vector<MakerFields> orders = {{nullptr, {"code", "message", "text"}}};
      bool checked = call->type->isResult();
      llvm::Value *make = llvm::ConstantPointerNull::get(ptr);
      if (checked) {
        make = this->lowering.objectMaker().maker(call->type->errType(), orders);
      }
      Lowering::Operand text = this->lowering.refCounts().emitOperand(arg, true);
      llvm::Value *result = this->callReturningDecimal(
          "tz_dec_from_string", {this->lowering.stringLowerer().stringObj(text.value),
                                 this->lowering.stringLowerer().stringMeta(text.value), make});
      this->lowering.refCounts().dropOperand(text);
      if (checked) {
        return this->lowering.resultLowerer().catchFailure(result, call->type, call->loc());
      }
      return result;
    }
    case Builtin::DecMax:
    case Builtin::DecMin:
    case Builtin::DecSum:
      return this->emitDecimalFold(call);
    default:
      break;
  }

  auto *member = cast<MemberExpr>(call->callee);
  Lowering::Operand self = this->lowering.refCounts().emitOperand(
      member->object, llvm::all_of(call->args, AssignLowerer::cannotAssign));
  llvm::Value *a = this->decimalCoef(self.value);
  llvm::Value *am = this->decimalMeta(self.value);
  auto count = [&](size_t i, int64_t absent) -> std::pair<llvm::Value *, llvm::Value *> {
    if (i < call->args.size()) {
      return {this->lowering.exprLowerer().emitExpr(call->args[i]),
              this->lowering.builder.getInt32(1)};
    }

    return {this->lowering.builder.getInt64(absent), this->lowering.builder.getInt32(0)};
  };
  auto text = [&](llvm::StringRef name, llvm::ArrayRef<llvm::Value *> args) {
    std::vector<llvm::Type *> params{ptr};
    for (llvm::Value *arg : args) {
      params.push_back(arg->getType());
    }

    return this->lowering.stringLowerer().callReturningString(
        this->decimalFunction(name, voidTy, params), args);
  };
  llvm::Value *result = nullptr;
  switch (call->builtin()) {
    case Builtin::DecDiv: {
      bool later =
          std::all_of(call->args.begin() + 1, call->args.end(), AssignLowerer::cannotAssign);
      Lowering::Operand divisor = this->lowering.refCounts().emitOperand(call->args[0], later);
      llvm::Value *dp = count(1, kDivisionPlaces).first;
      llvm::Value *mode = this->emitRoundingMode(call, 2);
      result =
          this->callReturningDecimal("tz_dec_div", {a, am, this->decimalCoef(divisor.value),
                                                    this->decimalMeta(divisor.value), dp, mode});
      this->lowering.refCounts().dropOperand(divisor);
      break;
    }
    case Builtin::DecRound: {
      llvm::Value *dp = count(0, 0).first;
      result =
          this->callReturningDecimal("tz_dec_round", {a, am, dp, this->emitRoundingMode(call, 1)});
      break;
    }
    case Builtin::DecToFixed:
    case Builtin::DecToFormat:
    case Builtin::DecToExponential:
    case Builtin::DecToPrecision: {
      auto [n, given] = count(0, 0);
      llvm::Value *mode = this->emitRoundingMode(call, 1);
      llvm::StringRef name = call->builtin() == Builtin::DecToFixed    ? "tz_dec_to_fixed"
                             : call->builtin() == Builtin::DecToFormat ? "tz_dec_to_format"
                             : call->builtin() == Builtin::DecToExponential
                                 ? "tz_dec_to_exponential"
                                 : "tz_dec_to_precision";
      result = text(name, {a, am, n, given, mode});
      break;
    }
    case Builtin::DecToString:
      if (call->args.empty()) {
        result = text("tz_dec_to_string", {a, am});
      } else {
        result = text("tz_dec_to_string_base",
                      {a, am, this->lowering.exprLowerer().emitExpr(call->args[0])});
      }
      break;
    case Builtin::DecSqrt:
      result = this->callReturningDecimal("tz_dec_sqrt", {a, am});
      break;
    case Builtin::DecAbs:
      result = this->callReturningDecimal("tz_dec_abs", {a, am});
      break;
    case Builtin::DecIsInteger:
      result = this->lowering.builder.CreateICmpNE(
          this->lowering.builder.CreateCall(
              this->decimalFunction("tz_dec_is_integer", i32, {i64, i64}), {a, am}),
          this->lowering.builder.getInt32(0));
      break;
    case Builtin::DecDecimalPlaces:
      result = this->lowering.builder.CreateSExt(
          this->lowering.builder.CreateCall(
              this->decimalFunction("tz_dec_decimal_places", i32, {i64, i64}), {a, am}),
          i64);
      break;
    case Builtin::DecShiftedBy:
      result = this->callReturningDecimal(
          "tz_dec_shifted_by", {a, am, this->lowering.exprLowerer().emitExpr(call->args[0])});
      break;
    case Builtin::DecToFraction: {
      llvm::Value *maxCoef = this->lowering.builder.getInt64(0),
                  *maxMeta = this->lowering.builder.getInt64(0);
      Lowering::Operand limit{nullptr, false, nullptr};
      if (!call->args.empty()) {
        limit = this->lowering.refCounts().emitOperand(call->args[0], true);
        maxCoef = this->decimalCoef(limit.value);
        maxMeta = this->decimalMeta(limit.value);
      }
      result = this->lowering.builder.CreateCall(
          this->decimalFunction("tz_dec_to_fraction", ptr, {ptr, i64, i64, i64, i64, i32}),
          {this->lowering.typeDescriptors().typeDescriptor(Type::decimalTy()), a, am, maxCoef,
           maxMeta, this->lowering.builder.getInt32(!call->args.empty())},
          "fraction");
      if (limit.value) {
        this->lowering.refCounts().dropOperand(limit);
      }
      break;
    }
    default:
      llvm_unreachable("not a method of decimals");
  }
  this->lowering.refCounts().dropOperand(self);

  return result;
}

}
