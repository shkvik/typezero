// Decimals: values of two words, { i64 coef, i64 meta }, as runtime/decimal.c
// lays them out. A short decimal is `coef * 10^-scale` with the scale in the
// low bits of meta; a long one points to its digits, counted like a string
// when they are on the heap, or a constant in the executable for a long
// literal. Arithmetic, comparisons and the methods call the runtime, whose
// fast paths for short values (TZ_HOT) are inlined into the program, so that
// `total += price` is an addition with an overflow check.

#include "codegen/codegen.h"
#include "support/decimal_literal.h"

#include "llvm/ADT/STLExtras.h"
#include "llvm/IR/MDBuilder.h"

using llvm::cast;
using llvm::dyn_cast;

namespace tz {

namespace {

// The flags of meta; must match runtime/tzrt.h.
constexpr uint64_t kDecLong = uint64_t(1) << 62;
// The largest scale of a short decimal.
constexpr int64_t kDecScaleMax = 0x7fffffff;
// A literal is never counted (it lacks the heap flag); the count only makes
// it look alive to anyone inspecting memory, as for strings.
constexpr int64_t kLiteralRefCount = int64_t(1) << 62;
// What `/` rounds to: 20 places, half up, as BigNumber.js by default.
constexpr int64_t kDivisionPlaces = 20;
// The folds of runtime/decimal.c.
enum Fold : int32_t { kFoldSum, kFoldMax, kFoldMin };

} // namespace

llvm::FunctionCallee CodeGen::decimalFunction(llvm::StringRef name, llvm::Type *ret,
                                              llvm::ArrayRef<llvm::Type *> params) {
  llvm::FunctionCallee callee =
      module_->getOrInsertFunction(name, llvm::FunctionType::get(ret, params, false));
  cast<llvm::Function>(callee.getCallee())->addFnAttr(llvm::Attribute::NoUnwind);
  return callee;
}

llvm::Value *CodeGen::decimalCoef(llvm::Value *value) {
  return builder_.CreateExtractValue(value, 0);
}

llvm::Value *CodeGen::decimalMeta(llvm::Value *value) {
  return builder_.CreateExtractValue(value, 1);
}

llvm::Value *CodeGen::callReturningDecimal(llvm::StringRef name,
                                           llvm::ArrayRef<llvm::Value *> args) {
  std::vector<llvm::Type *> params{builder_.getPtrTy()};
  for (llvm::Value *arg : args)
    params.push_back(arg->getType());
  llvm::AllocaInst *out = createStackSlot(decTy_, "dec.out");
  std::vector<llvm::Value *> all{out};
  all.insert(all.end(), args.begin(), args.end());
  builder_.CreateCall(decimalFunction(name, builder_.getVoidTy(), params), all);
  return builder_.CreateLoad(decTy_, out);
}

// Only long decimals on the heap, whose meta has the sign bit set, are
// counted; the last release frees them in the runtime.
llvm::Function *CodeGen::decimalRefCount(bool retain) {
  llvm::Function *&fn = retain ? decRetainFn_ : decReleaseFn_;
  if (fn)
    return fn;
  auto *type = llvm::FunctionType::get(builder_.getVoidTy(), {decTy_}, false);
  fn = llvm::Function::Create(type, llvm::GlobalValue::InternalLinkage,
                              retain ? "tz.dec.retain" : "tz.dec.release", module_.get());
  fn->addFnAttr(llvm::Attribute::AlwaysInline);
  fn->addFnAttr(llvm::Attribute::NoUnwind);
  llvm::IRBuilder<> b(llvm::BasicBlock::Create(ctx_, "entry", fn));
  auto *countedBlock = llvm::BasicBlock::Create(ctx_, "counted", fn);
  auto *doneBlock = llvm::BasicBlock::Create(ctx_, "done", fn);
  llvm::Value *value = fn->getArg(0);
  llvm::Value *counted = b.CreateICmpSLT(b.CreateExtractValue(value, 1, "meta"), b.getInt64(0));
  b.CreateCondBr(counted, countedBlock, doneBlock);
  b.SetInsertPoint(countedBlock);
  llvm::Value *obj = b.CreateIntToPtr(b.CreateExtractValue(value, 0, "coef"), b.getPtrTy(), "obj");
  llvm::Value *count = b.CreateLoad(b.getInt64Ty(), obj, "rc");
  auto *freeBlock = retain ? nullptr : llvm::BasicBlock::Create(ctx_, "free", fn);
  if (parallel_) // a shared one, atomically (runtime/publish.c)
    countShared(b, obj, count, retain, freeBlock, doneBlock);
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
llvm::Constant *CodeGen::emitDecimalLiteral(const Expr *expr) {
  bool negative = false;
  if (auto *unary = dyn_cast<UnaryExpr>(expr); unary && unary->op == UnaryOp::Neg) {
    negative = true;
    expr = unary->operand;
  }
  DecimalLiteral value;
  if (auto *lit = dyn_cast<IntLitExpr>(expr))
    value =
        lit->text.empty() ? decimalOf(lit->value) : readDecimalLiteral(lit->text).value_or(value);
  else
    value = readDecimalLiteral(cast<FloatLitExpr>(expr)->text).value_or(value); // Sema read it
  llvm::Type *i64 = builder_.getInt64Ty();
  auto shortValue = [&](uint64_t magnitude, int64_t scale) {
    uint64_t coef = negative ? 0 - magnitude : magnitude;
    return llvm::ConstantStruct::get(
        decTy_, {llvm::ConstantInt::get(i64, coef), llvm::ConstantInt::get(i64, uint64_t(scale))});
  };
  // Below the range, a decimal is zero (as in BigNumber.js).
  if (value.isZero() || value.top() < kDecimalMinExp)
    return shortValue(0, 0);
  const std::string &digits = value.digits;
  int64_t exponent = value.exponent;
  if (digits.size() <= 19) {
    uint64_t magnitude = std::stoull(digits);
    uint64_t limit = negative ? uint64_t(1) << 63 : (uint64_t(1) << 63) - 1;
    if (exponent <= 0 && -exponent <= kDecScaleMax && magnitude <= limit)
      return shortValue(magnitude, -exponent);
    uint64_t power = 1;
    bool fits = exponent > 0 && exponent < 20;
    for (int64_t i = 0; fits && i < exponent; ++i)
      power *= 10;
    if (fits && magnitude <= limit / power)
      return shortValue(magnitude * power, 0);
  }
  // Base 10^9 digits, the lowest first.
  std::vector<uint32_t> limbs;
  for (size_t end = digits.size(); end > 0;) {
    size_t start = end >= 9 ? end - 9 : 0;
    limbs.push_back(static_cast<uint32_t>(std::stoul(digits.substr(start, end - start))));
    end = start;
  }
  llvm::Type *i32 = builder_.getInt32Ty();
  llvm::Constant *data = llvm::ConstantDataArray::get(ctx_, limbs);
  auto *layout = llvm::StructType::get(ctx_, {i64, i64, i32, i32, data->getType()});
  llvm::Constant *init = llvm::ConstantStruct::get(
      layout,
      {builder_.getInt64(kLiteralRefCount), llvm::ConstantInt::get(i64, uint64_t(exponent)),
       builder_.getInt32(negative), builder_.getInt32(static_cast<uint32_t>(limbs.size())), data});
  auto *global = new llvm::GlobalVariable(*module_, layout, /*isConstant=*/true,
                                          llvm::GlobalValue::PrivateLinkage, init, ".decimal");
  global->setUnnamedAddr(llvm::GlobalValue::UnnamedAddr::Global);
  global->setAlignment(llvm::Align(8));
  return llvm::ConstantStruct::get(
      decTy_, {llvm::ConstantExpr::getPtrToInt(global, i64), builder_.getInt64(kDecLong)});
}

llvm::Value *CodeGen::decimalFromInteger(llvm::Value *value) {
  llvm::Value *coef = builder_.CreateSExt(value, builder_.getInt64Ty());
  llvm::Value *result = builder_.CreateInsertValue(llvm::UndefValue::get(decTy_), coef, 0);
  return builder_.CreateInsertValue(result, builder_.getInt64(0), 1);
}

llvm::Value *CodeGen::emitDecimalArithmetic(BinaryOp op, llvm::Value *lhs, llvm::Value *rhs) {
  llvm::Value *a = decimalCoef(lhs), *am = decimalMeta(lhs);
  if (op == BinaryOp::Pow)
    return callReturningDecimal("tz_dec_pow", {a, am, rhs});
  llvm::Value *b = decimalCoef(rhs), *bm = decimalMeta(rhs);
  switch (op) {
  case BinaryOp::Add:
    return callReturningDecimal("tz_dec_add", {a, am, b, bm});
  case BinaryOp::Sub:
    return callReturningDecimal("tz_dec_sub", {a, am, b, bm});
  case BinaryOp::Mul:
    return callReturningDecimal("tz_dec_mul", {a, am, b, bm});
  case BinaryOp::Div:
    return callReturningDecimal("tz_dec_div", {a, am, b, bm, builder_.getInt64(kDivisionPlaces),
                                               builder_.getInt32(kRoundHalfUp)});
  case BinaryOp::Rem:
    return callReturningDecimal("tz_dec_rem", {a, am, b, bm});
  default:
    llvm_unreachable("not an operator of decimals");
  }
}

// Arithmetic may throw (division by zero, a value too large): checked after
// the operands are released.
llvm::Value *CodeGen::emitDecimalBinary(BinaryExpr *bin) {
  Operand a = emitOperand(bin->lhs, cannotAssign(bin->rhs));
  holdOperand(a, exprThrows(bin->rhs));
  Operand b = emitOperand(bin->rhs, /*canBorrow=*/true);
  takeOperand(a);
  bool arithmetic = false;
  llvm::Value *result;
  switch (bin->op) {
  case BinaryOp::Lt:
  case BinaryOp::Le:
  case BinaryOp::Gt:
  case BinaryOp::Ge: {
    llvm::Type *i64 = builder_.getInt64Ty();
    llvm::Value *order = builder_.CreateCall(
        decimalFunction("tz_dec_cmp", builder_.getInt32Ty(), {i64, i64, i64, i64}),
        {decimalCoef(a.value), decimalMeta(a.value), decimalCoef(b.value), decimalMeta(b.value)});
    result = emitComparison(bin->op, order, builder_.getInt32(0), Type::intTy());
    break;
  }
  default:
    result = emitDecimalArithmetic(bin->op, a.value, b.value);
    arithmetic = true;
    break;
  }
  dropOperand(a);
  dropOperand(b);
  emitThrowCheckIf(arithmetic);
  return result;
}

llvm::Value *CodeGen::emitDecimalCompound(BinaryOp op, llvm::Value *old, Expr *value) {
  Operand rhs = emitOperand(value, /*canBorrow=*/true);
  llvm::Value *result = emitDecimalArithmetic(op, old, rhs.value);
  dropOperand(rhs);
  emitThrowCheck();
  return result;
}

// `x++` and `--x`: the slot gets a new value, and `x++` gives the old one,
// with the slot's reference.
llvm::Value *CodeGen::emitDecimalUpdate(UpdateExpr *update, llvm::Value *address) {
  llvm::Value *old = builder_.CreateLoad(decTy_, address);
  llvm::Constant *one =
      llvm::ConstantStruct::get(decTy_, {builder_.getInt64(1), builder_.getInt64(0)});
  llvm::Value *updated =
      emitDecimalArithmetic(update->increment ? BinaryOp::Add : BinaryOp::Sub, old, one);
  emitThrowCheck(); // a value too large
  builder_.CreateStore(updated, address);
  if (!update->prefix)
    return old;
  releaseValue(old, Type::decimalTy());
  retainValue(updated, Type::decimalTy());
  return updated;
}

llvm::Value *CodeGen::decimalSign(llvm::Value *value) {
  llvm::Type *i64 = builder_.getInt64Ty();
  return builder_.CreateCall(decimalFunction("tz_dec_sign", builder_.getInt32Ty(), {i64, i64}),
                             {decimalCoef(value), decimalMeta(value)});
}

llvm::Value *CodeGen::emitRoundingMode(CallExpr *call, size_t index) {
  if (index >= call->args.size())
    return builder_.getInt32(kRoundHalfUp);
  Expr *arg = call->args[index];
  if (auto *lit = dyn_cast<StringLitExpr>(arg))
    return builder_.getInt32(static_cast<uint32_t>(decimalRoundingMode(lit->value)));
  Operand mode = emitOperand(arg, /*canBorrow=*/true);
  llvm::Value *result =
      builder_.CreateCall(decimalFunction("tz_dec_rounding_mode", builder_.getInt32Ty(),
                                          {builder_.getPtrTy(), builder_.getInt64Ty()}),
                          {stringObj(mode.value), stringMeta(mode.value)});
  dropOperand(mode);
  emitThrowCheck(); // a mode that does not exist
  return result;
}

// decimal.sum, max and min: the arguments are evaluated first, as for any
// call; then the runtime goes over the values and the arrays spread.
llvm::Value *CodeGen::emitDecimalFold(CallExpr *call) {
  int32_t op = call->builtin == Builtin::DecSum   ? kFoldSum
               : call->builtin == Builtin::DecMax ? kFoldMax
                                                  : kFoldMin;
  std::vector<llvm::Value *> values = emitHeldValues(call->args);
  std::vector<Operand> args;
  for (size_t i = 0; i < values.size(); ++i)
    args.push_back({values[i], true, call->args[i]->type});
  llvm::Type *i32 = builder_.getInt32Ty(), *ptr = builder_.getPtrTy();
  llvm::Value *acc = createStackSlot(decTy_, "fold.acc");
  builder_.CreateStore(llvm::Constant::getNullValue(decTy_), acc);
  llvm::Value *has = createStackSlot(i32, "fold.has");
  builder_.CreateStore(builder_.getInt32(op == kFoldSum), has);
  for (size_t i = 0; i < args.size(); ++i) {
    if (call->spreads(i))
      builder_.CreateCall(
          decimalFunction("tz_dec_fold_array", builder_.getVoidTy(), {ptr, ptr, ptr, i32}),
          {acc, has, args[i].value, builder_.getInt32(op)});
    else
      builder_.CreateCall(decimalFunction("tz_dec_fold", builder_.getVoidTy(),
                                          {ptr, ptr, ptr, builder_.getInt64Ty(), i32}),
                          {acc, has, spill(args[i].value, "fold.value"), builder_.getInt64(1),
                           builder_.getInt32(op)});
  }
  for (const Operand &arg : args)
    dropOperand(arg);
  emitThrowCheckIf(op == kFoldSum); // a sum too large
  if (op != kFoldSum) {
    // Sema proved that some value is there; the arrays could have been
    // emptied by the arguments after them.
    llvm::BasicBlock *emptyBlock = newBlock("fold.empty");
    llvm::BasicBlock *doneBlock = newBlock("fold.done");
    builder_.CreateCondBr(
        builder_.CreateICmpEQ(builder_.CreateLoad(i32, has), builder_.getInt32(0)), emptyBlock,
        doneBlock, llvm::MDBuilder(ctx_).createUnlikelyBranchWeights());
    switchTo(emptyBlock);
    builder_.CreateCall(decimalFunction("tz_dec_no_values", builder_.getVoidTy(), {i32}),
                        {builder_.getInt32(op)});
    emitRuntimeThrow();
    switchTo(doneBlock);
  }
  return builder_.CreateLoad(decTy_, acc);
}

llvm::Value *CodeGen::emitDecimalCall(CallExpr *call) {
  llvm::Type *i32 = builder_.getInt32Ty(), *i64 = builder_.getInt64Ty(), *ptr = builder_.getPtrTy();
  llvm::Type *voidTy = builder_.getVoidTy();
  switch (call->builtin) {
  case Builtin::DecimalOf: {
    // Sema made an integer argument a decimal already.
    Expr *arg = call->args[0];
    if (arg->type->isDecimal())
      return emitExpr(arg);
    if (arg->type->isFloat()) {
      llvm::Value *result = callReturningDecimal("tz_dec_from_f64", {emitExpr(arg)});
      emitThrowCheck(); // NaN and the infinities
      return result;
    }
    Operand text = emitOperand(arg, /*canBorrow=*/true);
    llvm::Value *result =
        callReturningDecimal("tz_dec_from_string", {stringObj(text.value), stringMeta(text.value)});
    dropOperand(text);
    emitThrowCheck(); // SyntaxError
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
  Operand self = emitOperand(member->object, llvm::all_of(call->args, cannotAssign));
  // Held in a slot only while an argument may throw; its words are used on
  // the normal path.
  holdOperand(self, llvm::any_of(call->args, [&](Expr *arg) { return exprThrows(arg); }));
  llvm::Value *a = decimalCoef(self.value), *am = decimalMeta(self.value);
  // An integer argument and whether it is given.
  auto count = [&](size_t i, int64_t absent) -> std::pair<llvm::Value *, llvm::Value *> {
    if (i < call->args.size())
      return {emitExpr(call->args[i]), builder_.getInt32(1)};
    return {builder_.getInt64(absent), builder_.getInt32(0)};
  };
  auto text = [&](llvm::StringRef name, llvm::ArrayRef<llvm::Value *> args) {
    std::vector<llvm::Type *> params{ptr};
    for (llvm::Value *arg : args)
      params.push_back(arg->getType());
    return callReturningString(decimalFunction(name, voidTy, params), args);
  };
  llvm::Value *result = nullptr;
  switch (call->builtin) {
  case Builtin::DecDiv: {
    bool later = std::all_of(call->args.begin() + 1, call->args.end(), cannotAssign);
    Operand divisor = emitOperand(call->args[0], later);
    llvm::Value *dp = count(1, kDivisionPlaces).first;
    llvm::Value *mode = emitRoundingMode(call, 2);
    result = callReturningDecimal(
        "tz_dec_div", {a, am, decimalCoef(divisor.value), decimalMeta(divisor.value), dp, mode});
    dropOperand(divisor);
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
    if (call->args.empty())
      result = text("tz_dec_to_string", {a, am});
    else
      result = text("tz_dec_to_string_base", {a, am, emitExpr(call->args[0])});
    break;
  case Builtin::DecSqrt:
    result = callReturningDecimal("tz_dec_sqrt", {a, am});
    break;
  case Builtin::DecAbs:
    result = callReturningDecimal("tz_dec_abs", {a, am});
    break;
  case Builtin::DecIsInteger:
    result = builder_.CreateICmpNE(
        builder_.CreateCall(decimalFunction("tz_dec_is_integer", i32, {i64, i64}), {a, am}),
        builder_.getInt32(0));
    break;
  case Builtin::DecDecimalPlaces:
    result =
        builder_.CreateCall(decimalFunction("tz_dec_decimal_places", i32, {i64, i64}), {a, am});
    break;
  case Builtin::DecShiftedBy:
    result = callReturningDecimal("tz_dec_shifted_by", {a, am, emitExpr(call->args[0])});
    break;
  case Builtin::DecToFraction: {
    llvm::Value *maxCoef = builder_.getInt64(0), *maxMeta = builder_.getInt64(0);
    Operand limit{nullptr, false, nullptr};
    if (!call->args.empty()) {
      limit = emitOperand(call->args[0], /*canBorrow=*/true);
      maxCoef = decimalCoef(limit.value);
      maxMeta = decimalMeta(limit.value);
    }
    result = builder_.CreateCall(
        decimalFunction("tz_dec_to_fraction", ptr, {ptr, i64, i64, i64, i64, i32}),
        {typeDescriptor(Type::decimalTy()), a, am, maxCoef, maxMeta,
         builder_.getInt32(!call->args.empty())},
        "fraction");
    if (limit.value)
      dropOperand(limit);
    break;
  }
  default:
    llvm_unreachable("not a method of decimals");
  }
  takeOperand(self);
  dropOperand(self);
  // Arguments out of range, a negative square root, a value too large.
  emitThrowCheckIf(call->builtin != Builtin::DecIsInteger &&
                   call->builtin != Builtin::DecDecimalPlaces && call->builtin != Builtin::DecAbs &&
                   !(call->builtin == Builtin::DecToString && call->args.empty()));
  return result;
}

} // namespace tz
