#include "compiler/library/decimal/decimal_lowerer/decimal_lowerer.h"
#include "compiler/library/decimal/decimal_literal/decimal_literal.h"
#include "compiler/lower/errors/may_throw/may_throw.h"
#include "compiler/lower/errors/throw_lowerer/throw_lowerer.h"
#include "compiler/lower/expr/assign_lowerer/assign_lowerer.h"
#include "compiler/lower/expr/binary_lowerer/binary_lowerer.h"
#include "compiler/lower/expr/expr_lowerer/expr_lowerer.h"
#include "compiler/lower/function_frame/function_frame.h"
#include "compiler/lower/layouts/type_descriptors/type_descriptors.h"
#include "compiler/lower/memory/ref_counts/ref_counts.h"
#include "compiler/lower/runtime_functions/runtime_functions.h"
#include "compiler/lower/values/string_lowerer/string_lowerer.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/IR/MDBuilder.h"

using llvm::BasicBlock;
using llvm::cast;
using llvm::dyn_cast;
using llvm::isa;
using llvm::Value;

namespace tz {

namespace {

// The flags of meta; must match runtime/core/tzrt/tzrt.h.
constexpr uint64_t kDecLong = uint64_t(1) << 62;

// The largest scale of a short decimal.
constexpr int64_t kDecScaleMax = 0x7fffffff;

// A literal is never counted (it lacks the heap flag); the count only makes
// it look alive to anyone inspecting memory, as for strings.
constexpr int64_t kLiteralRefCount = int64_t(1) << 62;

// What `/` rounds to: 20 places, half up, as BigNumber.js by default.
constexpr int64_t kDivisionPlaces = 20;

// The folds of runtime/library/decimal/decimal/decimal.c.
enum Fold : int32_t { kFoldSum, kFoldMax, kFoldMin };

} // namespace

DecimalLowerer::DecimalLowerer(Lowering &lowering) : lowering_(lowering) {}

llvm::FunctionCallee DecimalLowerer::decimalFunction(llvm::StringRef name, llvm::Type *ret,
                                                     llvm::ArrayRef<llvm::Type *> params) {
  llvm::FunctionCallee callee =
      lowering_.module_->getOrInsertFunction(name, llvm::FunctionType::get(ret, params, false));
  cast<llvm::Function>(callee.getCallee())->addFnAttr(llvm::Attribute::NoUnwind);

  return callee;
}

llvm::Value *DecimalLowerer::decimalCoef(llvm::Value *value) {
  return lowering_.builder_.CreateExtractValue(value, 0);
}

llvm::Value *DecimalLowerer::decimalMeta(llvm::Value *value) {
  return lowering_.builder_.CreateExtractValue(value, 1);
}

llvm::Value *DecimalLowerer::callReturningDecimal(llvm::StringRef name,
                                                  llvm::ArrayRef<llvm::Value *> args) {
  std::vector<llvm::Type *> params{lowering_.builder_.getPtrTy()};
  for (llvm::Value *arg : args) {
    params.push_back(arg->getType());
  }
  llvm::AllocaInst *out = lowering_.functionFrame().createStackSlot(lowering_.decTy_, "dec.out");
  std::vector<llvm::Value *> all{out};
  all.insert(all.end(), args.begin(), args.end());
  lowering_.builder_.CreateCall(decimalFunction(name, lowering_.builder_.getVoidTy(), params), all);

  return lowering_.builder_.CreateLoad(lowering_.decTy_, out);
}

// Only long decimals on the heap, whose meta has the sign bit set, are
// counted; the last release frees them in the runtime.
llvm::Function *DecimalLowerer::decimalRefCount(bool retain) {
  llvm::Function *&fn = retain ? decRetainFn_ : decReleaseFn_;
  if (fn) {
    return fn;
  }
  auto *type = llvm::FunctionType::get(lowering_.builder_.getVoidTy(), {lowering_.decTy_}, false);
  fn = llvm::Function::Create(type, llvm::GlobalValue::InternalLinkage,
                              retain ? "tz.dec.retain" : "tz.dec.release", lowering_.module_.get());
  fn->addFnAttr(llvm::Attribute::AlwaysInline);
  fn->addFnAttr(llvm::Attribute::NoUnwind);
  llvm::IRBuilder<> b(llvm::BasicBlock::Create(lowering_.ctx_, "entry", fn));
  auto *countedBlock = llvm::BasicBlock::Create(lowering_.ctx_, "counted", fn);
  auto *doneBlock = llvm::BasicBlock::Create(lowering_.ctx_, "done", fn);
  llvm::Value *value = fn->getArg(0);
  llvm::Value *counted = b.CreateICmpSLT(b.CreateExtractValue(value, 1, "meta"), b.getInt64(0));
  b.CreateCondBr(counted, countedBlock, doneBlock);
  b.SetInsertPoint(countedBlock);
  llvm::Value *obj = b.CreateIntToPtr(b.CreateExtractValue(value, 0, "coef"), b.getPtrTy(), "obj");
  llvm::Value *count = b.CreateLoad(b.getInt64Ty(), obj, "rc");
  auto *freeBlock = retain ? nullptr : llvm::BasicBlock::Create(lowering_.ctx_, "free", fn);
  if (lowering_.parallel_) { // a shared one, atomically (runtime/core/publish/publish.c)
    lowering_.runtimeFunctions().countShared(b, obj, count, retain, freeBlock, doneBlock);
  }
  if (retain) {
    b.CreateStore(b.CreateAdd(count, b.getInt64(1)), obj);
    b.CreateBr(doneBlock);
  } else {
    llvm::Value *left = b.CreateSub(count, b.getInt64(1));
    b.CreateStore(left, obj);
    b.CreateCondBr(b.CreateICmpEQ(left, b.getInt64(0)), freeBlock, doneBlock);
    b.SetInsertPoint(freeBlock);
    b.CreateCall(decimalFunction("tz_dec_free", b.getVoidTy(), {b.getPtrTy()}), {obj});
    b.CreateBr(doneBlock);
  }
  b.SetInsertPoint(doneBlock);
  b.CreateRetVoid();

  return fn;
}

// A literal read from the text of the program: short if it fits, otherwise
// a constant TzDecObj { rc, exp, neg, n, digits } in the executable.
llvm::Constant *DecimalLowerer::emitDecimalLiteral(const Expr *expr) {
  bool negative = false;
  if (auto *unary = dyn_cast<UnaryExpr>(expr); unary && unary->op == UnaryOp::Neg) {
    negative = true;
    expr = unary->operand;
  }
  DecimalLiteral value;
  if (auto *lit = dyn_cast<IntLitExpr>(expr)) {
    value =
        lit->text.empty() ? decimalOf(lit->value) : readDecimalLiteral(lit->text).value_or(value);
  } else {
    value = readDecimalLiteral(cast<FloatLitExpr>(expr)->text).value_or(value); // Checker read it
  }
  llvm::Type *i64 = lowering_.builder_.getInt64Ty();
  auto shortValue = [&](uint64_t magnitude, int64_t scale) {
    uint64_t coef = negative ? 0 - magnitude : magnitude;

    return llvm::ConstantStruct::get(
        lowering_.decTy_,
        {llvm::ConstantInt::get(i64, coef), llvm::ConstantInt::get(i64, uint64_t(scale))});
  };
  // Below the range, a decimal is zero (as in BigNumber.js).
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
  // Base 10^9 digits, the lowest first.
  std::vector<uint32_t> limbs;
  for (size_t end = digits.size(); end > 0;) {
    size_t start = end >= 9 ? end - 9 : 0;
    limbs.push_back(static_cast<uint32_t>(std::stoul(digits.substr(start, end - start))));
    end = start;
  }
  llvm::Type *i32 = lowering_.builder_.getInt32Ty();
  llvm::Constant *data = llvm::ConstantDataArray::get(lowering_.ctx_, limbs);
  auto *layout = llvm::StructType::get(lowering_.ctx_, {i64, i64, i32, i32, data->getType()});
  llvm::Constant *init = llvm::ConstantStruct::get(
      layout,
      {lowering_.builder_.getInt64(kLiteralRefCount),
       llvm::ConstantInt::get(i64, uint64_t(exponent)), lowering_.builder_.getInt32(negative),
       lowering_.builder_.getInt32(static_cast<uint32_t>(limbs.size())), data});
  auto *global = new llvm::GlobalVariable(*lowering_.module_, layout, /*isConstant=*/true,
                                          llvm::GlobalValue::PrivateLinkage, init, ".decimal");
  global->setUnnamedAddr(llvm::GlobalValue::UnnamedAddr::Global);
  global->setAlignment(llvm::Align(8));

  return llvm::ConstantStruct::get(lowering_.decTy_, {llvm::ConstantExpr::getPtrToInt(global, i64),
                                                      lowering_.builder_.getInt64(kDecLong)});
}

llvm::Value *DecimalLowerer::decimalFromInteger(llvm::Value *value) {
  llvm::Value *coef = lowering_.builder_.CreateSExt(value, lowering_.builder_.getInt64Ty());
  llvm::Value *result =
      lowering_.builder_.CreateInsertValue(llvm::UndefValue::get(lowering_.decTy_), coef, 0);

  return lowering_.builder_.CreateInsertValue(result, lowering_.builder_.getInt64(0), 1);
}

llvm::Value *DecimalLowerer::emitDecimalArithmetic(BinaryOp op, llvm::Value *lhs,
                                                   llvm::Value *rhs) {
  llvm::Value *a = decimalCoef(lhs), *am = decimalMeta(lhs);
  if (op == BinaryOp::Pow) {
    return callReturningDecimal("tz_dec_pow", {a, am, rhs});
  }
  llvm::Value *b = decimalCoef(rhs), *bm = decimalMeta(rhs);
  switch (op) {
  case BinaryOp::Add:
    return callReturningDecimal("tz_dec_add", {a, am, b, bm});
  case BinaryOp::Sub:
    return callReturningDecimal("tz_dec_sub", {a, am, b, bm});
  case BinaryOp::Mul:
    return callReturningDecimal("tz_dec_mul", {a, am, b, bm});
  case BinaryOp::Div:
    return callReturningDecimal("tz_dec_div",
                                {a, am, b, bm, lowering_.builder_.getInt64(kDivisionPlaces),
                                 lowering_.builder_.getInt32(kRoundHalfUp)});
  case BinaryOp::Rem:
    return callReturningDecimal("tz_dec_rem", {a, am, b, bm});
  default:
    llvm_unreachable("not an operator of decimals");
  }
}

// Arithmetic may throw (division by zero, a value too large): checked after
// the operands are released.
llvm::Value *DecimalLowerer::emitDecimalBinary(BinaryExpr *bin) {
  Lowering::Operand a =
      lowering_.refCounts().emitOperand(bin->lhs, AssignLowerer::cannotAssign(bin->rhs));
  lowering_.throwLowerer().holdOperand(a, lowering_.mayThrow().exprThrows(bin->rhs));
  Lowering::Operand b = lowering_.refCounts().emitOperand(bin->rhs, /*canBorrow=*/true);
  lowering_.throwLowerer().takeOperand(a);
  bool arithmetic = false;
  llvm::Value *result;
  switch (bin->op) {
  case BinaryOp::Lt:
  case BinaryOp::Le:
  case BinaryOp::Gt:
  case BinaryOp::Ge: {
    llvm::Type *i64 = lowering_.builder_.getInt64Ty();
    llvm::Value *order = lowering_.builder_.CreateCall(
        decimalFunction("tz_dec_cmp", lowering_.builder_.getInt32Ty(), {i64, i64, i64, i64}),
        {decimalCoef(a.value), decimalMeta(a.value), decimalCoef(b.value), decimalMeta(b.value)});
    result = lowering_.binaryLowerer().emitComparison(
        bin->op, order, lowering_.builder_.getInt32(0), Type::intTy());
    break;
  }
  default:
    result = emitDecimalArithmetic(bin->op, a.value, b.value);
    arithmetic = true;
    break;
  }
  lowering_.refCounts().dropOperand(a);
  lowering_.refCounts().dropOperand(b);
  lowering_.emitThrowCheckIf(arithmetic);

  return result;
}

llvm::Value *DecimalLowerer::emitDecimalCompound(BinaryOp op, llvm::Value *old, Expr *value) {
  Lowering::Operand rhs = lowering_.refCounts().emitOperand(value, /*canBorrow=*/true);
  llvm::Value *result = emitDecimalArithmetic(op, old, rhs.value);
  lowering_.refCounts().dropOperand(rhs);
  lowering_.throwLowerer().emitThrowCheck();

  return result;
}

// `x++` and `--x`: the slot gets a new value, and `x++` gives the old one,
// with the slot's reference.
llvm::Value *DecimalLowerer::emitDecimalUpdate(UpdateExpr *update, llvm::Value *address) {
  llvm::Value *old = lowering_.builder_.CreateLoad(lowering_.decTy_, address);
  llvm::Constant *one = llvm::ConstantStruct::get(
      lowering_.decTy_, {lowering_.builder_.getInt64(1), lowering_.builder_.getInt64(0)});
  llvm::Value *updated =
      emitDecimalArithmetic(update->increment ? BinaryOp::Add : BinaryOp::Sub, old, one);
  lowering_.throwLowerer().emitThrowCheck(); // a value too large
  lowering_.builder_.CreateStore(updated, address);
  if (!update->prefix) {
    return old;
  }
  lowering_.refCounts().releaseValue(old, Type::decimalTy());
  lowering_.refCounts().retainValue(updated, Type::decimalTy());

  return updated;
}

llvm::Value *DecimalLowerer::decimalSign(llvm::Value *value) {
  llvm::Type *i64 = lowering_.builder_.getInt64Ty();

  return lowering_.builder_.CreateCall(
      decimalFunction("tz_dec_sign", lowering_.builder_.getInt32Ty(), {i64, i64}),
      {decimalCoef(value), decimalMeta(value)});
}

llvm::Value *DecimalLowerer::emitRoundingMode(CallExpr *call, size_t index) {
  if (index >= call->args.size()) {
    return lowering_.builder_.getInt32(kRoundHalfUp);
  }
  Expr *arg = call->args[index];
  if (auto *lit = dyn_cast<StringLitExpr>(arg)) {
    return lowering_.builder_.getInt32(static_cast<uint32_t>(decimalRoundingMode(lit->value)));
  }
  Lowering::Operand mode = lowering_.refCounts().emitOperand(arg, /*canBorrow=*/true);
  llvm::Value *result = lowering_.builder_.CreateCall(
      decimalFunction("tz_dec_rounding_mode", lowering_.builder_.getInt32Ty(),
                      {lowering_.builder_.getPtrTy(), lowering_.builder_.getInt64Ty()}),
      {lowering_.stringLowerer().stringObj(mode.value),
       lowering_.stringLowerer().stringMeta(mode.value)});
  lowering_.refCounts().dropOperand(mode);
  lowering_.throwLowerer().emitThrowCheck(); // a mode that does not exist

  return result;
}

// decimal.sum, max and min: the arguments are evaluated first, as for any
// call; then the runtime goes over the values and the arrays spread.
llvm::Value *DecimalLowerer::emitDecimalFold(CallExpr *call) {
  int32_t op = call->builtin == Builtin::DecSum   ? kFoldSum
               : call->builtin == Builtin::DecMax ? kFoldMax
                                                  : kFoldMin;
  std::vector<llvm::Value *> values = lowering_.throwLowerer().emitHeldValues(call->args);
  std::vector<Lowering::Operand> args;
  for (size_t i = 0; i < values.size(); ++i) {
    args.push_back({values[i], true, call->args[i]->type});
  }
  llvm::Type *i32 = lowering_.builder_.getInt32Ty(), *ptr = lowering_.builder_.getPtrTy();
  llvm::Value *acc = lowering_.functionFrame().createStackSlot(lowering_.decTy_, "fold.acc");
  lowering_.builder_.CreateStore(llvm::Constant::getNullValue(lowering_.decTy_), acc);
  llvm::Value *has = lowering_.functionFrame().createStackSlot(i32, "fold.has");
  lowering_.builder_.CreateStore(lowering_.builder_.getInt32(op == kFoldSum), has);
  for (size_t i = 0; i < args.size(); ++i) {
    if (call->spreads(i)) {
      lowering_.builder_.CreateCall(decimalFunction("tz_dec_fold_array",
                                                    lowering_.builder_.getVoidTy(),
                                                    {ptr, ptr, ptr, i32}),
                                    {acc, has, args[i].value, lowering_.builder_.getInt32(op)});
    } else {
      lowering_.builder_.CreateCall(
          decimalFunction("tz_dec_fold", lowering_.builder_.getVoidTy(),
                          {ptr, ptr, ptr, lowering_.builder_.getInt64Ty(), i32}),
          {acc, has, lowering_.functionFrame().spill(args[i].value, "fold.value"),
           lowering_.builder_.getInt64(1), lowering_.builder_.getInt32(op)});
    }
  }
  for (const Lowering::Operand &arg : args) {
    lowering_.refCounts().dropOperand(arg);
  }
  lowering_.emitThrowCheckIf(op == kFoldSum); // a sum too large
  if (op != kFoldSum) {
    // Checker proved that some value is there; the arrays could have been
    // emptied by the arguments after them.
    llvm::BasicBlock *emptyBlock = lowering_.functionFrame().newBlock("fold.empty");
    llvm::BasicBlock *doneBlock = lowering_.functionFrame().newBlock("fold.done");
    lowering_.builder_.CreateCondBr(
        lowering_.builder_.CreateICmpEQ(lowering_.builder_.CreateLoad(i32, has),
                                        lowering_.builder_.getInt32(0)),
        emptyBlock, doneBlock, llvm::MDBuilder(lowering_.ctx_).createUnlikelyBranchWeights());
    lowering_.functionFrame().switchTo(emptyBlock);
    lowering_.builder_.CreateCall(
        decimalFunction("tz_dec_no_values", lowering_.builder_.getVoidTy(), {i32}),
        {lowering_.builder_.getInt32(op)});
    lowering_.throwLowerer().emitRuntimeThrow();
    lowering_.functionFrame().switchTo(doneBlock);
  }

  return lowering_.builder_.CreateLoad(lowering_.decTy_, acc);
}

llvm::Value *DecimalLowerer::emitDecimalCall(CallExpr *call) {
  llvm::Type *i32 = lowering_.builder_.getInt32Ty(), *i64 = lowering_.builder_.getInt64Ty(),
             *ptr = lowering_.builder_.getPtrTy();
  llvm::Type *voidTy = lowering_.builder_.getVoidTy();
  switch (call->builtin) {
  case Builtin::DecimalOf: {
    // Checker made an integer argument a decimal already.
    Expr *arg = call->args[0];
    if (arg->type->isDecimal()) {
      return lowering_.exprLowerer().emitExpr(arg);
    }
    if (arg->type->isFloat()) {
      llvm::Value *result =
          callReturningDecimal("tz_dec_from_f64", {lowering_.exprLowerer().emitExpr(arg)});
      lowering_.throwLowerer().emitThrowCheck(); // NaN and the infinities
      return result;
    }
    Lowering::Operand text = lowering_.refCounts().emitOperand(arg, /*canBorrow=*/true);
    llvm::Value *result = callReturningDecimal("tz_dec_from_string",
                                               {lowering_.stringLowerer().stringObj(text.value),
                                                lowering_.stringLowerer().stringMeta(text.value)});
    lowering_.refCounts().dropOperand(text);
    lowering_.throwLowerer().emitThrowCheck(); // SyntaxError
    return result;
  }
  case Builtin::DecMax:
  case Builtin::DecMin:
  case Builtin::DecSum:
    return emitDecimalFold(call);
  default:
    break;
  }

  // A method: the receiver, then the arguments in order.
  auto *member = cast<MemberExpr>(call->callee);
  Lowering::Operand self = lowering_.refCounts().emitOperand(
      member->object, llvm::all_of(call->args, AssignLowerer::cannotAssign));
  // Held in a slot only while an argument may throw; its words are used on
  // the normal path.
  lowering_.throwLowerer().holdOperand(self, llvm::any_of(call->args, [&](Expr *arg) {
                                         return lowering_.mayThrow().exprThrows(arg);
                                       }));
  llvm::Value *a = decimalCoef(self.value), *am = decimalMeta(self.value);
  // An integer argument and whether it is given.
  auto count = [&](size_t i, int64_t absent) -> std::pair<llvm::Value *, llvm::Value *> {
    if (i < call->args.size()) {
      return {lowering_.exprLowerer().emitExpr(call->args[i]), lowering_.builder_.getInt32(1)};
    }

    return {lowering_.builder_.getInt64(absent), lowering_.builder_.getInt32(0)};
  };
  auto text = [&](llvm::StringRef name, llvm::ArrayRef<llvm::Value *> args) {
    std::vector<llvm::Type *> params{ptr};
    for (llvm::Value *arg : args) {
      params.push_back(arg->getType());
    }

    return lowering_.stringLowerer().callReturningString(decimalFunction(name, voidTy, params),
                                                         args);
  };
  llvm::Value *result = nullptr;
  switch (call->builtin) {
  case Builtin::DecDiv: {
    bool later = std::all_of(call->args.begin() + 1, call->args.end(), AssignLowerer::cannotAssign);
    Lowering::Operand divisor = lowering_.refCounts().emitOperand(call->args[0], later);
    llvm::Value *dp = count(1, kDivisionPlaces).first;
    llvm::Value *mode = emitRoundingMode(call, 2);
    result = callReturningDecimal(
        "tz_dec_div", {a, am, decimalCoef(divisor.value), decimalMeta(divisor.value), dp, mode});
    lowering_.refCounts().dropOperand(divisor);
    break;
  }
  case Builtin::DecRound: {
    llvm::Value *dp = count(0, 0).first;
    result = callReturningDecimal("tz_dec_round", {a, am, dp, emitRoundingMode(call, 1)});
    break;
  }
  case Builtin::DecToFixed:
  case Builtin::DecToFormat:
  case Builtin::DecToExponential:
  case Builtin::DecToPrecision: {
    auto [n, given] = count(0, 0);
    llvm::Value *mode = emitRoundingMode(call, 1);
    llvm::StringRef name = call->builtin == Builtin::DecToFixed         ? "tz_dec_to_fixed"
                           : call->builtin == Builtin::DecToFormat      ? "tz_dec_to_format"
                           : call->builtin == Builtin::DecToExponential ? "tz_dec_to_exponential"
                                                                        : "tz_dec_to_precision";
    result = text(name, {a, am, n, given, mode});
    break;
  }
  case Builtin::DecToString:
    // Without a base, String(x); with one, rounded as BigNumber.js rounds.
    if (call->args.empty()) {
      result = text("tz_dec_to_string", {a, am});
    } else {
      result =
          text("tz_dec_to_string_base", {a, am, lowering_.exprLowerer().emitExpr(call->args[0])});
    }
    break;
  case Builtin::DecSqrt:
    result = callReturningDecimal("tz_dec_sqrt", {a, am});
    break;
  case Builtin::DecAbs:
    result = callReturningDecimal("tz_dec_abs", {a, am});
    break;
  case Builtin::DecIsInteger:
    result = lowering_.builder_.CreateICmpNE(
        lowering_.builder_.CreateCall(decimalFunction("tz_dec_is_integer", i32, {i64, i64}),
                                      {a, am}),
        lowering_.builder_.getInt32(0));
    break;
  case Builtin::DecDecimalPlaces:
    result = lowering_.builder_.CreateCall(
        decimalFunction("tz_dec_decimal_places", i32, {i64, i64}), {a, am});
    break;
  case Builtin::DecShiftedBy:
    result = callReturningDecimal("tz_dec_shifted_by",
                                  {a, am, lowering_.exprLowerer().emitExpr(call->args[0])});
    break;
  case Builtin::DecToFraction: {
    llvm::Value *maxCoef = lowering_.builder_.getInt64(0),
                *maxMeta = lowering_.builder_.getInt64(0);
    Lowering::Operand limit{nullptr, false, nullptr};
    if (!call->args.empty()) {
      limit = lowering_.refCounts().emitOperand(call->args[0], /*canBorrow=*/true);
      maxCoef = decimalCoef(limit.value);
      maxMeta = decimalMeta(limit.value);
    }
    result = lowering_.builder_.CreateCall(
        decimalFunction("tz_dec_to_fraction", ptr, {ptr, i64, i64, i64, i64, i32}),
        {lowering_.typeDescriptors().typeDescriptor(Type::decimalTy()), a, am, maxCoef, maxMeta,
         lowering_.builder_.getInt32(!call->args.empty())},
        "fraction");
    if (limit.value) {
      lowering_.refCounts().dropOperand(limit);
    }
    break;
  }
  default:
    llvm_unreachable("not a method of decimals");
  }
  lowering_.throwLowerer().takeOperand(self);
  lowering_.refCounts().dropOperand(self);
  // Arguments out of range, a negative square root, a value too large.
  lowering_.emitThrowCheckIf(call->builtin != Builtin::DecIsInteger &&
                             call->builtin != Builtin::DecDecimalPlaces &&
                             call->builtin != Builtin::DecAbs &&
                             !(call->builtin == Builtin::DecToString && call->args.empty()));

  return result;
}

} // namespace tz
