#include "codegen/codegen.h"

#include "llvm/ADT/STLExtras.h"
#include "llvm/IR/Intrinsics.h"
#include "llvm/IR/MDBuilder.h"
#include "llvm/Support/ConvertUTF.h"
#include "llvm/Support/ErrorHandling.h"

#include <array>

using llvm::cast;
using llvm::dyn_cast;
using llvm::isa;

namespace tz {

namespace {

// String value layout; must match runtime/string.c.
constexpr uint64_t kStrLengthMask = (uint64_t(1) << 30) - 1;
constexpr uint64_t kStrAscii = uint64_t(1) << 60;
constexpr uint64_t kStrTwoByte = uint64_t(1) << 61;
constexpr uint64_t kStrRope = uint64_t(1) << 62;
constexpr uint64_t kStrHeap = uint64_t(1) << 63;
// A one-byte string of up to this many code units is stored in the value
// itself: `obj` holds the code units and `meta` has ROPE without HEAP.
constexpr size_t kInlineMax = 8;
// A number passed to tz_str_concat_n as it is (see runtime/string.c): the
// ROPE and TWO_BYTE flags without HEAP, and the kind of number in the low bits.
constexpr uint64_t kNumberPart = kStrRope | kStrTwoByte;
constexpr uint64_t kNumberInt = 1;
constexpr uint64_t kNumberDouble = 2;
constexpr uint32_t kKindBuffer1 = 1;
constexpr uint32_t kKindBuffer2 = 2;
// Literals are never counted (their values lack the HEAP flag); a huge count
// just makes them look alive to anyone inspecting memory.
constexpr int64_t kLiteralRefCount = int64_t(1) << 62;

// Collects the operands of a chain of string `+`. Concatenation is
// associative, so a + (b + c) flattens as well as (a + b) + c.
void collectConcat(Expr *expr, std::vector<Expr *> &parts) {
  auto *bin = dyn_cast<BinaryExpr>(expr);
  if (bin && bin->op == BinaryOp::Add && bin->type->isString()) {
    collectConcat(bin->lhs, parts);
    collectConcat(bin->rhs, parts);
  } else {
    parts.push_back(expr);
  }
}

} // namespace

CodeGen::CodeGen(llvm::LLVMContext &ctx, llvm::StringRef moduleName, const llvm::DataLayout &layout)
    : ctx_(ctx), module_(std::make_unique<llvm::Module>(moduleName, ctx)), builder_(ctx) {
  module_->setDataLayout(layout);
  llvm::Type *ptr = builder_.getPtrTy();
  llvm::Type *i32 = builder_.getInt32Ty();
  llvm::Type *i64 = builder_.getInt64Ty();
  strTy_ = llvm::StructType::create(ctx, {ptr, i64}, "tz.string");
  arrayTy_ = llvm::StructType::create(ctx, {i64, ptr, ptr, i32, i32, ptr}, "tz.array");
  typeDescTy_ = llvm::StructType::create(ctx, {i32, i32, ptr}, "tz.type");
  fnTy_ = llvm::StructType::create(ctx, {ptr, ptr}, "tz.fn");
  fnInfoTy_ = llvm::StructType::create(ctx, {ptr, strTy_}, "tz.fninfo");
  fieldDescTy_ = llvm::StructType::create(ctx, {ptr, i32, i32, ptr}, "tz.field");
  layoutDescTy_ = llvm::StructType::create(ctx, {ptr, ptr, i32}, "tz.layout");
}

std::unique_ptr<llvm::Module> CodeGen::generate(SourceFile &file) {
  comparesFunctions_ = file.comparesFunctions;
  declareRuntime();
  defineRefCountHelpers();
  std::vector<FunctionDecl *> functions;
  for (Stmt *item : file.items)
    if (auto *fn = dyn_cast<FunctionDecl>(item)) {
      declareFunction(fn);
      functions.push_back(fn);
    }
  findTailGroups(functions);
  declareGlobals(file);
  for (Stmt *item : file.items)
    if (auto *fn = dyn_cast<FunctionDecl>(item))
      emitFunction(fn);
  emitMain(file);
  // The closures each function creates come after it.
  emitPendingClosures();
  return std::move(module_);
}

llvm::Type *CodeGen::lower(const Type *type) {
  switch (type->kind()) {
  case Type::Kind::Void: return builder_.getVoidTy();
  case Type::Kind::Undefined: return builder_.getInt1Ty(); // never read
  case Type::Kind::Bool: return builder_.getInt1Ty();
  case Type::Kind::Int: return builder_.getInt32Ty();
  case Type::Kind::Long: return builder_.getInt64Ty();
  case Type::Kind::Double: return builder_.getDoubleTy();
  case Type::Kind::String: return strTy_;
  case Type::Kind::Array: return builder_.getPtrTy();
  case Type::Kind::Optional:
    if (type->inner()->isObject()) // null is undefined
      return builder_.getPtrTy();
    return llvm::StructType::get(ctx_, {lower(type->inner()), builder_.getInt1Ty()});
  case Type::Kind::Function: return fnTy_;
  case Type::Kind::Object: return builder_.getPtrTy();
  case Type::Kind::Error: break;
  }
  llvm_unreachable("an erroneous type reached code generation");
}

//===----------------------------------------------------------------------===//
// Module structure
//===----------------------------------------------------------------------===//

void CodeGen::declareRuntime() {
  llvm::Type *voidTy = builder_.getVoidTy();
  llvm::Type *i32 = builder_.getInt32Ty();
  llvm::Type *i64 = builder_.getInt64Ty();
  llvm::Type *f64 = builder_.getDoubleTy();
  llvm::Type *ptr = builder_.getPtrTy();

  auto declare = [&](llvm::StringRef name, llvm::Type *ret, llvm::ArrayRef<llvm::Type *> params) {
    llvm::FunctionCallee callee =
        module_->getOrInsertFunction(name, llvm::FunctionType::get(ret, params, false));
    cast<llvm::Function>(callee.getCallee())->addFnAttr(llvm::Attribute::NoUnwind);
    return callee;
  };
  rt_.init = declare("tz_rt_init", voidTy, {});
  rt_.shutdown = declare("tz_rt_shutdown", voidTy, {});
  rt_.mainDone = declare("tz_rt_main_done", voidTy, {});
  rt_.printI32 = declare("tz_print_i32", voidTy, {i32});
  rt_.printI64 = declare("tz_print_i64", voidTy, {i64});
  rt_.printF64 = declare("tz_print_f64", voidTy, {f64});
  rt_.printBool = declare("tz_print_bool", voidTy, {i32});
  rt_.printChar = declare("tz_print_char", voidTy, {i32});
  rt_.printString = declare("tz_print_string", voidTy, {ptr, i64});
  rt_.printValue = declare("tz_print_value", voidTy, {ptr, ptr});
  rt_.panic = declare("tz_panic", voidTy, {ptr, i64});
  rt_.debugger = declare("tz_debugger", voidTy, {});
  // Numbers (runtime/tzrt.c, number.c and math.c).
  rt_.toInt32 = declare("tz_to_int32", i32, {f64});
  rt_.mathRound = declare("tz_math_round", f64, {f64});
  rt_.mathPow = declare("tz_math_pow", f64, {f64, f64});
  rt_.mathPowLong = declare("tz_math_pow_long", i64, {i64, i64});
  rt_.mathHypot = declare("tz_math_hypot", f64, {ptr, i64});
  rt_.mathRandom = declare("tz_math_random", f64, {});
  rt_.checkedInt32 = declare("tz_checked_int32", i32, {f64});
  rt_.checkedInt64 = declare("tz_checked_int64", i64, {f64});
  rt_.checkedInt32OfLong = declare("tz_checked_int32_of_long", i32, {i64});
  rt_.strToNumber = declare("tz_str_to_number", f64, {ptr, i64});
  rt_.parseInt = declare("tz_parse_int", f64, {ptr, i64, i32});
  rt_.parseFloat = declare("tz_parse_float", f64, {ptr, i64});
  rt_.numToFixed = declare("tz_num_to_fixed", voidTy, {ptr, f64, i64});

  // String functions take strings as (obj, meta) pairs and return them
  // through a pointer passed first.
  rt_.strFree = declare("tz_str_free", voidTy, {ptr});
  rt_.strConcatN = declare("tz_str_concat_n", voidTy, {ptr, ptr, i64});
  rt_.strEqual = declare("tz_str_equal", i32, {ptr, i64, ptr, i64});
  rt_.strCompare = declare("tz_str_compare", i32, {ptr, i64, ptr, i64});
  rt_.strCharAt = declare("tz_str_char_at", voidTy, {ptr, ptr, i64, i64});
  rt_.strCharCodeAt = declare("tz_str_char_code_at", i32, {ptr, i64, i64});
  rt_.strCodePointAt = declare("tz_str_code_point_at", i32, {ptr, ptr, i64, i64});
  rt_.strSlice = declare("tz_str_slice", voidTy, {ptr, ptr, i64, i64, i64, i32});
  rt_.strSubstring = declare("tz_str_substring", voidTy, {ptr, ptr, i64, i64, i64, i32});
  rt_.strIndexOf = declare("tz_str_index_of", i32, {ptr, i64, ptr, i64, i64, i32});
  rt_.strIncludes = declare("tz_str_includes", i32, {ptr, i64, ptr, i64, i64, i32});
  rt_.strStartsWith = declare("tz_str_starts_with", i32, {ptr, i64, ptr, i64, i64, i32});
  rt_.strEndsWith = declare("tz_str_ends_with", i32, {ptr, i64, ptr, i64, i64, i32});
  rt_.strRepeat = declare("tz_str_repeat", voidTy, {ptr, ptr, i64, i64});
  rt_.strFromI64 = declare("tz_str_from_i64", voidTy, {ptr, i64});
  rt_.strFromF64 = declare("tz_str_from_f64", voidTy, {ptr, f64});
  rt_.valueToString = declare("tz_value_to_string", voidTy, {ptr, ptr, ptr});
  rt_.strCharAtOrEmpty = declare("tz_str_char_at_or_empty", voidTy, {ptr, ptr, i64, i64});
  rt_.strCodePoint = declare("tz_str_code_point", i32, {ptr, i64, i64});
  rt_.strLastIndexOf = declare("tz_str_last_index_of", i32, {ptr, i64, ptr, i64, i64, i32});
  rt_.strSplit = declare("tz_str_split", ptr, {ptr, ptr, i64, ptr, i64, i32, i64, i32});
  rt_.strTrim = declare("tz_str_trim", voidTy, {ptr, ptr, i64, i32});
  rt_.strPad = declare("tz_str_pad", voidTy, {ptr, ptr, i64, i64, ptr, i64, i32, i32});
  rt_.strToUpper = declare("tz_str_to_upper", voidTy, {ptr, ptr, i64});
  rt_.strToLower = declare("tz_str_to_lower", voidTy, {ptr, ptr, i64});
  rt_.strReplace = declare("tz_str_replace", voidTy, {ptr, ptr, i64, ptr, i64, ptr, i64, i32});
  rt_.strFromCharCodes = declare("tz_str_from_char_codes", voidTy, {ptr, ptr, i64});
  rt_.strFromCodePoints = declare("tz_str_from_code_points", voidTy, {ptr, ptr, i64});

  // Arrays (runtime/array.c). Values of any type go by address, with the
  // type descriptor of the elements stored in the array.
  rt_.arrNew = declare("tz_arr_new", ptr, {ptr, i64, i64});
  rt_.arrNewLength = declare("tz_arr_new_length", ptr, {ptr, i64});
  rt_.arrFree = declare("tz_arr_free", voidTy, {ptr});
  rt_.arrGrow = declare("tz_arr_grow", voidTy, {ptr, i64});
  rt_.arrShift = declare("tz_arr_shift", i32, {ptr, ptr});
  rt_.arrUnshiftSlots = declare("tz_arr_unshift_slots", ptr, {ptr, i64});
  rt_.arrSetLength = declare("tz_arr_set_length", voidTy, {ptr, i64});
  rt_.arrSlice = declare("tz_arr_slice", ptr, {ptr, i64, i64, i32});
  rt_.arrCopy = declare("tz_arr_copy", ptr, {ptr});
  rt_.arrAppend = declare("tz_arr_append", voidTy, {ptr, ptr});
  rt_.arrAppendCodePoints = declare("tz_arr_append_code_points", voidTy, {ptr, ptr, i64});
  rt_.arrIndexOf = declare("tz_arr_index_of", i32, {ptr, ptr, i64, i32, i32});
  rt_.arrJoin = declare("tz_arr_join", voidTy, {ptr, ptr, ptr, i64, i32});
  rt_.arrReverse = declare("tz_arr_reverse", voidTy, {ptr});
  rt_.arrFill = declare("tz_arr_fill", voidTy, {ptr, ptr, i64, i64, i32, i32});
  rt_.arrSort = declare("tz_arr_sort", voidTy, {ptr});
  rt_.arrSortWith = declare("tz_arr_sort_with", voidTy, {ptr, ptr, ptr});
  rt_.arrSplice = declare("tz_arr_splice", ptr, {ptr, i64, i64, i32, i64, ptr});
  // The objects of closures (runtime/closure.c).
  rt_.objAlloc = declare("tz_obj_alloc", ptr, {i64});
  rt_.objFree = declare("tz_obj_free", voidTy, {ptr});

  // Functions that return memory nothing else points to yet.
  for (llvm::FunctionCallee fresh : {rt_.arrNew, rt_.arrNewLength, rt_.arrSlice, rt_.arrCopy, rt_.objAlloc})
    cast<llvm::Function>(fresh.getCallee())->addRetAttr(llvm::Attribute::NoAlias);

  auto *panic = cast<llvm::Function>(rt_.panic.getCallee());
  panic->addFnAttr(llvm::Attribute::NoReturn);
  panic->addFnAttr(llvm::Attribute::Cold);
  cast<llvm::Function>(rt_.arrGrow.getCallee())->addFnAttr(llvm::Attribute::Cold);
}

// The inline fast paths of reference counting:
// - tz.str.retain / tz.str.release: only heap strings, whose meta has the sign
//   bit set, are counted;
// - tz.arr.retain / tz.arr.release: every array is counted; null (the zero
//   value of a slot) is skipped. tz.arr.retain serves objects too, whose
//   count is the first word as well (see codegen_object.cpp for releasing
//   them).
void CodeGen::defineRefCountHelpers() {
  for (bool isString : {true, false}) {
    llvm::Type *param = isString ? static_cast<llvm::Type *>(strTy_) : builder_.getPtrTy();
    auto *type = llvm::FunctionType::get(builder_.getVoidTy(), {param}, false);
    for (bool isRetain : {true, false}) {
      std::string name = std::string(isString ? "tz.str." : "tz.arr.") + (isRetain ? "retain" : "release");
      auto *fn = llvm::Function::Create(type, llvm::GlobalValue::InternalLinkage, name, module_.get());
      fn->addFnAttr(llvm::Attribute::AlwaysInline);
      fn->addFnAttr(llvm::Attribute::NoUnwind);

      llvm::IRBuilder<> b(llvm::BasicBlock::Create(ctx_, "entry", fn));
      auto *countedBlock = llvm::BasicBlock::Create(ctx_, "counted", fn);
      auto *doneBlock = llvm::BasicBlock::Create(ctx_, "done", fn);
      llvm::Value *obj, *counted;
      if (isString) {
        llvm::Value *str = fn->getArg(0);
        obj = b.CreateExtractValue(str, 0, "obj");
        counted = b.CreateICmpSLT(b.CreateExtractValue(str, 1, "meta"), b.getInt64(0));
      } else {
        obj = fn->getArg(0);
        counted = b.CreateIsNotNull(obj);
      }
      b.CreateCondBr(counted, countedBlock, doneBlock);

      b.SetInsertPoint(countedBlock);
      llvm::Value *count = b.CreateLoad(b.getInt64Ty(), obj, "rc");
      if (isRetain) {
        b.CreateStore(b.CreateAdd(count, b.getInt64(1)), obj);
        b.CreateBr(doneBlock);
      } else {
        llvm::Value *left = b.CreateSub(count, b.getInt64(1));
        b.CreateStore(left, obj);
        auto *freeBlock = llvm::BasicBlock::Create(ctx_, "free", fn);
        b.CreateCondBr(b.CreateICmpEQ(left, b.getInt64(0)), freeBlock, doneBlock);
        b.SetInsertPoint(freeBlock);
        b.CreateCall(isString ? rt_.strFree : rt_.arrFree, {obj});
        b.CreateBr(doneBlock);
      }

      b.SetInsertPoint(doneBlock);
      b.CreateRetVoid();
      if (isString)
        (isRetain ? retainFn_ : releaseFn_) = fn;
      else
        (isRetain ? arrRetainFn_ : arrReleaseFn_) = fn;
    }
  }

  // tz.obj.retain / tz.obj.release: the objects of closures, which start with
  // { i64 rc, ptr drop }; the last release calls their drop function.
  auto *objHeader = llvm::StructType::get(ctx_, {builder_.getInt64Ty(), builder_.getPtrTy()});
  auto *type = llvm::FunctionType::get(builder_.getVoidTy(), {builder_.getPtrTy()}, false);
  for (bool isRetain : {true, false}) {
    auto *fn = llvm::Function::Create(type, llvm::GlobalValue::InternalLinkage,
                                      isRetain ? "tz.obj.retain" : "tz.obj.release", module_.get());
    fn->addFnAttr(llvm::Attribute::AlwaysInline);
    fn->addFnAttr(llvm::Attribute::NoUnwind);
    llvm::IRBuilder<> b(llvm::BasicBlock::Create(ctx_, "entry", fn));
    auto *countedBlock = llvm::BasicBlock::Create(ctx_, "counted", fn);
    auto *doneBlock = llvm::BasicBlock::Create(ctx_, "done", fn);
    llvm::Value *obj = fn->getArg(0);
    b.CreateCondBr(b.CreateIsNotNull(obj), countedBlock, doneBlock);
    b.SetInsertPoint(countedBlock);
    llvm::Value *count = b.CreateLoad(b.getInt64Ty(), obj, "rc");
    if (isRetain) {
      b.CreateStore(b.CreateAdd(count, b.getInt64(1)), obj);
      b.CreateBr(doneBlock);
    } else {
      llvm::Value *left = b.CreateSub(count, b.getInt64(1));
      b.CreateStore(left, obj);
      auto *dropBlock = llvm::BasicBlock::Create(ctx_, "drop", fn);
      b.CreateCondBr(b.CreateICmpEQ(left, b.getInt64(0)), dropBlock, doneBlock);
      b.SetInsertPoint(dropBlock);
      llvm::Value *drop = b.CreateLoad(b.getPtrTy(), b.CreateStructGEP(objHeader, obj, 1), "drop");
      b.CreateCall(type, drop, {obj});
      b.CreateBr(doneBlock);
    }
    b.SetInsertPoint(doneBlock);
    b.CreateRetVoid();
    (isRetain ? objRetainFn_ : objReleaseFn_) = fn;
  }
}

void CodeGen::setFunctionAttributes(llvm::Function *fn) {
  // Nothing unwinds yet, but unwind tables still let debuggers and crash
  // handlers walk the stack.
  fn->addFnAttr(llvm::Attribute::NoUnwind);
  fn->setUWTableKind(llvm::UWTableKind::Default);
}

void CodeGen::declareFunction(FunctionDecl *fn) {
  const FuncSymbol *sym = fn->sym;
  std::vector<llvm::Type *> params;
  for (const Type *param : sym->params)
    params.push_back(lower(param));
  auto *type = llvm::FunctionType::get(lower(sym->ret), params, false);
  // The whole program is one module, so everything but `main` is internal and
  // LLVM is free to inline or drop it.
  auto *function = llvm::Function::Create(type, llvm::GlobalValue::InternalLinkage,
                                          "tz." + fn->name, module_.get());
  setFunctionAttributes(function);
  functions_[sym] = function;
}

void CodeGen::declareGlobals(SourceFile &file) {
  for (Stmt *item : file.items) {
    auto *decl = dyn_cast<VarDeclStmt>(item);
    if (!decl)
      continue;
    for (VarDeclarator &d : decl->decls) {
      if (!d.sym->usedInFunction)
        continue; // a local of main
      llvm::Type *type = lower(d.sym->type);
      // Holds zero (or the empty string) until the top-level code that
      // declares it runs.
      globals_[d.sym] = new llvm::GlobalVariable(*module_, type, /*isConstant=*/false,
                                                 llvm::GlobalValue::InternalLinkage,
                                                 llvm::Constant::getNullValue(type), "tz." + d.name);
      if (d.sym->type->isManaged())
        managedGlobals_.push_back(d.sym);
    }
  }
}

void CodeGen::beginFunction(llvm::Function *function, FunctionDecl *fn) {
  function_ = function;
  currentFn_ = fn;
  returnType_ = fn ? fn->sym->ret : nullptr;
  envParam_ = nullptr;
  addresses_.clear();
  boxSlots_.clear();
  envBoxes_.clear();
  groupEnvs_.clear();
  builder_.SetInsertPoint(llvm::BasicBlock::Create(ctx_, "entry", function_));
}

void CodeGen::emitFunction(FunctionDecl *fn) {
  if (TailGroup *group = tailGroupOf_.lookup(fn)) {
    emitTailGroup(*group);
    return;
  }
  beginFunction(functions_.lookup(fn->sym), fn);
  std::vector<llvm::Value *> args;
  for (llvm::Argument &arg : function_->args())
    args.push_back(&arg);
  emitParamsAndBody(fn, args);
  finishFunction();
}

// The parameters, from the arguments given, and the body.
void CodeGen::emitParamsAndBody(FunctionDecl *fn, llvm::ArrayRef<llvm::Value *> args) {
  pushScope(); // parameters: the callee owns its arguments
  for (size_t i = 0; i < fn->params.size(); ++i) {
    const Param &param = fn->params[i];
    llvm::Value *arg = args[i];
    if (isa<llvm::Argument>(arg))
      arg->setName(param.name);
    // Parameters can be assigned to, so they live in stack slots like locals.
    llvm::Value *slot = createLocal(param.sym, param.name);
    if (!param.defaultValue) {
      builder_.CreateStore(arg, slot);
      continue;
    }
    // The argument is `T | undefined`; undefined takes the default value.
    const Type *passed = Type::optional(param.sym->type);
    llvm::BasicBlock *givenBlock = newBlock("param.given");
    llvm::BasicBlock *defaultBlock = newBlock("param.default");
    llvm::BasicBlock *doneBlock = newBlock("param.done");
    builder_.CreateCondBr(optionalHas(arg, passed), givenBlock, defaultBlock);
    switchTo(givenBlock);
    llvm::Value *given = param.sym->type->isOptional() ? arg : optionalValue(arg, passed);
    builder_.CreateBr(doneBlock);
    switchTo(defaultBlock);
    llvm::Value *fallback = emitExpr(param.defaultValue);
    llvm::BasicBlock *defaultEnd = builder_.GetInsertBlock();
    builder_.CreateBr(doneBlock);
    switchTo(doneBlock);
    llvm::PHINode *value = builder_.CreatePHI(lower(param.sym->type), 2);
    value->addIncoming(given, givenBlock);
    value->addIncoming(fallback, defaultEnd);
    builder_.CreateStore(value, slot);
  }
  emitStmt(fn->body);
  popScope();
}

void CodeGen::emitMain(SourceFile &file) {
  auto *type = llvm::FunctionType::get(builder_.getInt32Ty(), false);
  auto *main = llvm::Function::Create(type, llvm::GlobalValue::ExternalLinkage, "main", module_.get());
  setFunctionAttributes(main);
  beginFunction(main, nullptr);
  builder_.CreateCall(rt_.init);
  pushScope();
  for (Stmt *item : file.items)
    if (!isa<FunctionDecl>(item))
      emitStmt(item);
  // Exits right away unless a leak check wants everything released.
  if (blockIsOpen())
    builder_.CreateCall(rt_.mainDone);
  popScope();
  if (blockIsOpen()) {
    // Release the globals too, so that a leak check finds every count at zero.
    for (const VarSymbol *var : llvm::reverse(managedGlobals_)) {
      llvm::Type *lowered = lower(var->type);
      llvm::Value *address = addressOf(var);
      llvm::Value *value = builder_.CreateLoad(lowered, address);
      builder_.CreateStore(llvm::Constant::getNullValue(lowered), address);
      releaseValue(value, var->type);
    }
    builder_.CreateCall(rt_.shutdown);
  }
  finishFunction();
}

void CodeGen::finishFunction() {
  closeBody();
  function_ = nullptr;
}

// Control that reaches the end of a body returns.
void CodeGen::closeBody() {
  if (blockIsOpen()) {
    llvm::Type *ret = function_->getReturnType();
    if (ret->isVoidTy())
      builder_.CreateRetVoid();
    else if (function_->hasExternalLinkage()) // main
      builder_.CreateRet(builder_.getInt32(0));
    else if (returnType_ && returnType_->isOptional()) // falls off the end: undefined
      builder_.CreateRet(undefinedValue(returnType_));
    else // Sema proved that every path returns
      builder_.CreateUnreachable();
  }
}

//===----------------------------------------------------------------------===//
// Blocks and scopes
//===----------------------------------------------------------------------===//

llvm::BasicBlock *CodeGen::newBlock(const llvm::Twine &name) {
  return llvm::BasicBlock::Create(ctx_, name);
}

void CodeGen::switchTo(llvm::BasicBlock *block) {
  function_->insert(function_->end(), block);
  builder_.SetInsertPoint(block);
}

void CodeGen::emitBlock(llvm::BasicBlock *block) {
  branchTo(block);
  switchTo(block);
}

bool CodeGen::blockIsOpen() { return !builder_.GetInsertBlock()->hasTerminator(); }

void CodeGen::branchTo(llvm::BasicBlock *block) {
  if (blockIsOpen())
    builder_.CreateBr(block);
}

void CodeGen::startDeadBlock() { switchTo(newBlock("dead")); }

void CodeGen::pushScope() { scopes_.emplace_back(); }

void CodeGen::popScope() {
  if (blockIsOpen())
    emitScopeCleanup(scopes_.back());
  scopes_.pop_back();
}

void CodeGen::emitCleanups(size_t depth) {
  for (size_t i = scopes_.size(); i > depth; --i)
    emitScopeCleanup(scopes_[i - 1]);
}

void CodeGen::emitScopeCleanup(const Scope &scope) {
  for (const auto &[slot, type] : llvm::reverse(scope.slots)) {
    if (!type) { // a box or an environment
      llvm::Value *object = builder_.CreateLoad(builder_.getPtrTy(), slot);
      builder_.CreateStore(llvm::ConstantPointerNull::get(builder_.getPtrTy()), slot);
      builder_.CreateCall(objReleaseFn_, {object});
      continue;
    }
    llvm::Type *lowered = lower(type);
    llvm::Value *value = builder_.CreateLoad(lowered, slot);
    // Emptied, so that a second cleanup of the same slot (a loop that leaves
    // early on the next iteration) does nothing.
    builder_.CreateStore(llvm::Constant::getNullValue(lowered), slot);
    releaseValue(value, type);
  }
}

llvm::AllocaInst *CodeGen::createStackSlot(llvm::Type *type, const llvm::Twine &name) {
  llvm::BasicBlock &entry = function_->getEntryBlock();
  llvm::IRBuilder<> entryBuilder(&entry, entry.begin());
  return entryBuilder.CreateAlloca(type, nullptr, name);
}

llvm::Value *CodeGen::createManagedSlot(const Type *type, const llvm::Twine &name) {
  llvm::Type *lowered = lower(type);
  llvm::AllocaInst *slot = createStackSlot(lowered, name);
  // Starts empty, so that releasing it before it was assigned is harmless.
  llvm::IRBuilder<> init(slot->getParent(), std::next(slot->getIterator()));
  init.CreateStore(llvm::Constant::getNullValue(lowered), slot);
  scopes_.back().slots.push_back({slot, type});
  return slot;
}

llvm::Value *CodeGen::createObjectSlot(const llvm::Twine &name) {
  llvm::AllocaInst *slot = createStackSlot(builder_.getPtrTy(), name);
  llvm::IRBuilder<> init(slot->getParent(), std::next(slot->getIterator()));
  init.CreateStore(llvm::ConstantPointerNull::get(builder_.getPtrTy()), slot);
  scopes_.back().slots.push_back({slot, nullptr});
  return slot;
}

llvm::Value *CodeGen::createLocal(const VarSymbol *var, llvm::StringRef name) {
  if (var->boxed()) {
    // Closures share the variable: it lives in a box, a new one each time
    // the declaration runs.
    llvm::Value *slot = createObjectSlot(name + ".box");
    boxSlots_[var] = slot;
    llvm::Value *box = newBox(typeOf(var));
    llvm::Value *old = builder_.CreateLoad(builder_.getPtrTy(), slot);
    builder_.CreateStore(box, slot);
    builder_.CreateCall(objReleaseFn_, {old});
    return builder_.CreateStructGEP(boxType(typeOf(var)), box, 2, name);
  }
  llvm::Value *slot = var->type->isManaged() ? createManagedSlot(var->type, name)
                                             : createStackSlot(lower(var->type), name);
  addresses_[var] = slot;
  return slot;
}

llvm::Value *CodeGen::addressOf(const VarSymbol *var) {
  if (var->boxed())
    return builder_.CreateStructGEP(boxType(typeOf(var)), boxPointer(var), 2);
  if (llvm::Value *address = addresses_.lookup(var))
    return address;
  llvm::Value *global = globals_.lookup(var);
  assert(global && "variable used before its declaration was emitted");
  return global;
}

llvm::Constant *CodeGen::utf8Constant(llvm::StringRef text) {
  auto [it, inserted] = utf8Constants_.try_emplace(text, nullptr);
  if (inserted)
    it->second = builder_.CreateGlobalString(text, ".msg", 0, module_.get());
  return it->second;
}

llvm::Value *CodeGen::spill(llvm::Value *value, const llvm::Twine &name) {
  llvm::AllocaInst *slot = createStackSlot(value->getType(), name);
  builder_.CreateStore(value, slot);
  return slot;
}

//===----------------------------------------------------------------------===//
// Reference counting
//===----------------------------------------------------------------------===//

void CodeGen::retainValue(llvm::Value *value, const Type *type) {
  if (isa<llvm::Constant>(value)) // literals, empty slots
    return;
  switch (type->kind()) {
  case Type::Kind::String:
    builder_.CreateCall(retainFn_, {value});
    break;
  case Type::Kind::Array:
    builder_.CreateCall(arrRetainFn_, {value});
    break;
  case Type::Kind::Function:
    builder_.CreateCall(objRetainFn_, {builder_.CreateExtractValue(value, 1)});
    break;
  case Type::Kind::Object: // the count is the first word, as for arrays
    builder_.CreateCall(arrRetainFn_, {value});
    break;
  case Type::Kind::Optional:
    // Undefined holds the zero value, which needs no counting.
    if (type->inner()->isManaged())
      retainValue(optionalValue(value, type), type->inner());
    break;
  default:
    break;
  }
}

void CodeGen::releaseValue(llvm::Value *value, const Type *type) {
  if (isa<llvm::Constant>(value))
    return;
  switch (type->kind()) {
  case Type::Kind::String:
    builder_.CreateCall(releaseFn_, {value});
    break;
  case Type::Kind::Array:
    builder_.CreateCall(arrReleaseFn_, {value});
    break;
  case Type::Kind::Function:
    builder_.CreateCall(objReleaseFn_, {builder_.CreateExtractValue(value, 1)});
    break;
  case Type::Kind::Object:
    builder_.CreateCall(objectLayout(type).release, {value});
    break;
  case Type::Kind::Optional:
    if (type->inner()->isManaged())
      releaseValue(optionalValue(value, type), type->inner());
    break;
  default:
    break;
  }
}

void CodeGen::retainString(llvm::Value *str) { retainValue(str, Type::string()); }

void CodeGen::releaseString(llvm::Value *str) { releaseValue(str, Type::string()); }

llvm::Value *CodeGen::makeOptional(llvm::Value *value, llvm::Value *has, const Type *type) {
  if (type->inner()->isObject()) {
    if (auto *always = dyn_cast<llvm::ConstantInt>(has); always && always->isOne())
      return value;
    return builder_.CreateSelect(has, value, llvm::ConstantPointerNull::get(builder_.getPtrTy()));
  }
  llvm::Value *result = llvm::UndefValue::get(lower(type));
  result = builder_.CreateInsertValue(result, value, 0);
  return builder_.CreateInsertValue(result, has, 1);
}

llvm::Value *CodeGen::undefinedValue(const Type *optionalType) {
  return llvm::Constant::getNullValue(lower(optionalType));
}

llvm::Value *CodeGen::optionalHas(llvm::Value *value, const Type *optionalType) {
  if (optionalType->inner()->isObject())
    return builder_.CreateIsNotNull(value);
  return builder_.CreateExtractValue(value, 1);
}

llvm::Value *CodeGen::optionalValue(llvm::Value *value, const Type *optionalType) {
  if (optionalType->inner()->isObject())
    return value;
  return builder_.CreateExtractValue(value, 0);
}

CodeGen::Operand CodeGen::emitOperand(Expr *expr, bool canBorrow) {
  if (canBorrow)
    if (llvm::Value *value = emitBorrowed(expr))
      return {value, false, expr->type};
  return {emitExpr(expr), true, expr->type};
}

// The variable keeps the objects on the way alive, and the caller made sure
// that nothing assigns to them, or to the variable, while the value is used.
llvm::Value *CodeGen::emitBorrowed(Expr *expr) {
  if (auto *name = dyn_cast<NameExpr>(expr); name && name->var)
    return emitVarValue(name->var, /*borrow=*/true);
  // A variable or a field narrowed to hold a value.
  if (auto *conv = dyn_cast<CastExpr>(expr); conv && conv->isImplicit() && !conv->shortCircuit &&
                                             conv->operand->type->isOptional() &&
                                             conv->type == conv->operand->type->inner()) {
    llvm::Value *value = emitBorrowed(conv->operand);
    return value ? optionalValue(value, conv->operand->type) : nullptr;
  }
  auto *member = dyn_cast<MemberExpr>(expr);
  if (!member || !member->object->type->isObject())
    return nullptr;
  llvm::Value *object = emitBorrowed(member->object);
  if (!object)
    return nullptr;
  return builder_.CreateLoad(lower(member->type), fieldAddress(object, member->object->type, member->name),
                             member->name);
}

void CodeGen::dropOperand(const Operand &operand) {
  if (operand.owned)
    releaseValue(operand.value, operand.type);
}

//===----------------------------------------------------------------------===//
// Statements
//===----------------------------------------------------------------------===//

void CodeGen::emitStmt(Stmt *stmt) {
  switch (stmt->kind()) {
  case Stmt::Kind::Block:
    pushScope();
    emitStatements(cast<BlockStmt>(stmt));
    popScope();
    break;
  case Stmt::Kind::VarDecl:
    emitVarDecl(cast<VarDeclStmt>(stmt));
    break;
  case Stmt::Kind::Function:
    break; // emitted on its own
  case Stmt::Kind::Expr:
    if (tailStatements_.contains(stmt))
      emitTailJump(cast<CallExpr>(cast<ExprStmt>(stmt)->expr));
    else
      emitDiscarded(cast<ExprStmt>(stmt)->expr);
    break;
  case Stmt::Kind::If:
    emitIf(cast<IfStmt>(stmt));
    break;
  case Stmt::Kind::While:
    emitWhile(cast<WhileStmt>(stmt));
    break;
  case Stmt::Kind::DoWhile:
    emitDoWhile(cast<DoWhileStmt>(stmt));
    break;
  case Stmt::Kind::For:
    emitFor(cast<ForStmt>(stmt));
    break;
  case Stmt::Kind::ForOf:
    emitForOf(cast<ForOfStmt>(stmt));
    break;
  case Stmt::Kind::Switch:
    emitSwitch(cast<SwitchStmt>(stmt));
    break;
  case Stmt::Kind::Labeled:
    emitLabeled(cast<LabeledStmt>(stmt));
    break;
  case Stmt::Kind::Return:
    emitReturn(cast<ReturnStmt>(stmt));
    break;
  case Stmt::Kind::Break:
  case Stmt::Kind::Continue:
    emitJump(stmt);
    break;
  case Stmt::Kind::Empty:
  case Stmt::Kind::TypeAlias:
  case Stmt::Kind::Enum:
    break;
  case Stmt::Kind::Debugger:
    builder_.CreateCall(rt_.debugger);
    break;
  }
}

void CodeGen::emitStatements(BlockStmt *block) {
  NestedGroup *group = block->group;
  for (size_t i = 0; i < block->body.size(); ++i) {
    if (group && i == group->firstUse)
      emitGroupCreation(group);
    emitStmt(block->body[i]);
  }
}

void CodeGen::pushLoop(llvm::BasicBlock *breakTarget, llvm::BasicBlock *continueTarget,
                       size_t scopeDepth) {
  targets_.push_back(
      {std::move(pendingLabels_), /*isLoop=*/true, /*isSwitch=*/false, breakTarget, continueTarget, scopeDepth});
  pendingLabels_.clear();
}

void CodeGen::emitJump(Stmt *stmt) {
  bool isBreak = isa<BreakStmt>(stmt);
  llvm::StringRef label = isBreak ? cast<BreakStmt>(stmt)->label : cast<ContinueStmt>(stmt)->label;
  const JumpTarget *target = nullptr;
  for (const JumpTarget &t : llvm::reverse(targets_)) {
    bool matches = !label.empty() ? llvm::is_contained(t.labels, label)
                   : isBreak      ? t.isLoop || t.isSwitch
                                  : t.isLoop;
    if (matches) {
      target = &t;
      break;
    }
  }
  assert(target && "Sema checks every break and continue");
  emitCleanups(target->scopeDepth);
  builder_.CreateBr(isBreak ? target->breakTarget : target->continueTarget);
  startDeadBlock();
}

void CodeGen::emitLabeled(LabeledStmt *stmt) {
  llvm::SmallVector<llvm::StringRef, 1> labels;
  Stmt *body = stmt;
  while (auto *labeled = dyn_cast<LabeledStmt>(body)) {
    labels.push_back(labeled->label);
    body = labeled->body;
  }
  if (isa<WhileStmt>(body) || isa<DoWhileStmt>(body) || isa<ForStmt>(body) ||
      isa<ForOfStmt>(body) || isa<SwitchStmt>(body)) {
    pendingLabels_ = labels; // the loop or switch takes them
    emitStmt(body);
    return;
  }
  // `label: { ... break label; ... }`
  llvm::BasicBlock *endBlock = newBlock("label.end");
  targets_.push_back({labels, /*isLoop=*/false, /*isSwitch=*/false, endBlock, nullptr, scopes_.size()});
  emitStmt(body);
  targets_.pop_back();
  emitBlock(endBlock);
}

// The discriminant is evaluated once and compared with each test in order;
// the first one equal to it jumps to its case, and no match to `default`.
// The case bodies follow each other, so control falls through.
void CodeGen::emitSwitch(SwitchStmt *stmt) {
  const Type *type = stmt->discriminant->type;
  llvm::SmallVector<llvm::StringRef, 1> labels = std::move(pendingLabels_);
  pendingLabels_.clear();
  size_t depth = scopes_.size();
  pushScope(); // the variables of the cases
  llvm::Value *value = emitExpr(stmt->discriminant);
  llvm::Value *slot = nullptr;
  if (type->isManaged()) {
    // Released whichever way control leaves the switch.
    slot = createManagedSlot(type, "switch.value");
    builder_.CreateStore(value, slot);
  }

  llvm::BasicBlock *exitBlock = newBlock("switch.exit");
  llvm::BasicBlock *endBlock = newBlock("switch.end");
  std::vector<llvm::BasicBlock *> bodies;
  llvm::BasicBlock *defaultBody = exitBlock;
  for (const SwitchCase &c : stmt->cases) {
    bodies.push_back(newBlock(c.test ? "switch.case" : "switch.default"));
    if (!c.test)
      defaultBody = bodies.back();
  }

  for (size_t i = 0; i < stmt->cases.size(); ++i) {
    const SwitchCase &c = stmt->cases[i];
    if (!c.test)
      continue;
    llvm::Value *test = emitExpr(c.test);
    llvm::Value *current = slot ? builder_.CreateLoad(lower(type), slot) : value;
    llvm::Value *equal;
    if (!c.compareType) // `case undefined:`
      equal = type->isOptional()    ? builder_.CreateNot(optionalHas(current, type))
              : type->isUndefined() ? builder_.getTrue()
                                    : builder_.getFalse();
    else
      equal = emitValuesEqual(emitConversion(current, type, c.compareType), test, c.compareType);
    releaseValue(test, c.test->type);
    llvm::BasicBlock *next = newBlock("switch.test");
    builder_.CreateCondBr(equal, bodies[i], next);
    switchTo(next);
  }
  builder_.CreateBr(defaultBody);

  targets_.push_back({labels, /*isLoop=*/false, /*isSwitch=*/true, endBlock, nullptr, depth});
  for (size_t i = 0; i < stmt->cases.size(); ++i) {
    switchTo(bodies[i]);
    for (Stmt *s : stmt->cases[i].body)
      emitStmt(s);
    branchTo(i + 1 < bodies.size() ? bodies[i + 1] : exitBlock);
  }
  targets_.pop_back();
  switchTo(exitBlock);
  popScope();
  emitBlock(endBlock);
}

void CodeGen::storeReplacing(llvm::Value *address, llvm::Value *value, const Type *type) {
  if (!type->isManaged()) {
    builder_.CreateStore(value, address);
    return;
  }
  llvm::Value *previous = builder_.CreateLoad(lower(type), address);
  builder_.CreateStore(value, address);
  releaseValue(previous, type);
}

void CodeGen::emitVarDecl(VarDeclStmt *decl) {
  for (VarDeclarator &d : decl->decls) {
    const VarSymbol *var = d.sym;
    llvm::Type *type = lower(var->type);
    bool global = var->isGlobal && var->usedInFunction;
    llvm::Value *address = global ? addressOf(var) : createLocal(var, d.name);
    llvm::Value *value = d.init ? emitExpr(d.init) : llvm::Constant::getNullValue(type);
    // The slot may still hold a value from an earlier run of this declaration.
    storeReplacing(address, value, var->type);
  }
}

void CodeGen::emitIf(IfStmt *stmt) {
  llvm::Value *cond = emitExpr(stmt->cond);
  llvm::BasicBlock *thenBlock = newBlock("if.then");
  llvm::BasicBlock *endBlock = newBlock("if.end");
  llvm::BasicBlock *elseBlock = stmt->elseStmt ? newBlock("if.else") : endBlock;
  builder_.CreateCondBr(cond, thenBlock, elseBlock);

  switchTo(thenBlock);
  emitStmt(stmt->thenStmt);
  branchTo(endBlock);

  if (stmt->elseStmt) {
    switchTo(elseBlock);
    emitStmt(stmt->elseStmt);
    branchTo(endBlock);
  }
  switchTo(endBlock);
}

void CodeGen::emitWhile(WhileStmt *stmt) {
  llvm::BasicBlock *condBlock = newBlock("while.cond");
  llvm::BasicBlock *bodyBlock = newBlock("while.body");
  llvm::BasicBlock *endBlock = newBlock("while.end");

  emitBlock(condBlock);
  llvm::Value *cond = emitExpr(stmt->cond);
  builder_.CreateCondBr(cond, bodyBlock, endBlock);

  switchTo(bodyBlock);
  pushLoop(endBlock, condBlock, scopes_.size());
  emitStmt(stmt->body);
  targets_.pop_back();
  branchTo(condBlock);

  switchTo(endBlock);
}

void CodeGen::emitDoWhile(DoWhileStmt *stmt) {
  llvm::BasicBlock *bodyBlock = newBlock("do.body");
  llvm::BasicBlock *condBlock = newBlock("do.cond");
  llvm::BasicBlock *endBlock = newBlock("do.end");

  emitBlock(bodyBlock);
  pushLoop(endBlock, condBlock, scopes_.size());
  emitStmt(stmt->body);
  targets_.pop_back();

  emitBlock(condBlock);
  llvm::Value *cond = emitExpr(stmt->cond);
  builder_.CreateCondBr(cond, bodyBlock, endBlock);

  switchTo(endBlock);
}

void CodeGen::emitFor(ForStmt *stmt) {
  pushScope(); // variables declared in the loop header
  if (stmt->init)
    emitStmt(stmt->init);

  llvm::BasicBlock *condBlock = newBlock("for.cond");
  llvm::BasicBlock *bodyBlock = newBlock("for.body");
  llvm::BasicBlock *stepBlock = newBlock("for.step");
  llvm::BasicBlock *endBlock = newBlock("for.end");

  emitBlock(condBlock);
  if (stmt->cond) {
    llvm::Value *cond = emitExpr(stmt->cond);
    builder_.CreateCondBr(cond, bodyBlock, endBlock);
  } else {
    builder_.CreateBr(bodyBlock);
  }

  switchTo(bodyBlock);
  pushLoop(endBlock, stepBlock, scopes_.size());
  emitStmt(stmt->body);
  targets_.pop_back();

  emitBlock(stepBlock);
  // `for (let i ...)`: every iteration has an i of its own. A closure that
  // captured i keeps that iteration's box; the next iteration goes on in a
  // copy.
  if (auto *decl = llvm::dyn_cast_or_null<VarDeclStmt>(stmt->init))
    for (VarDeclarator &d : decl->decls) {
      if (!d.sym->boxed())
        continue;
      const Type *type = typeOf(d.sym);
      llvm::Value *slot = boxSlots_.lookup(d.sym);
      llvm::Value *old = builder_.CreateLoad(builder_.getPtrTy(), slot);
      llvm::Value *box = newBox(type);
      llvm::Value *value = builder_.CreateLoad(lower(type), builder_.CreateStructGEP(boxType(type), old, 2));
      retainValue(value, type);
      builder_.CreateStore(value, builder_.CreateStructGEP(boxType(type), box, 2));
      builder_.CreateStore(box, slot);
      builder_.CreateCall(objReleaseFn_, {old});
    }
  if (stmt->step)
    emitDiscarded(stmt->step);
  builder_.CreateBr(condBlock);

  switchTo(endBlock);
  popScope();
}

// `for (const x of iterable)`: the iterable is evaluated once and held in a
// hidden slot; each step reads the current length, as JavaScript's iterators
// do, so elements pushed by the body are visited too.
void CodeGen::emitForOf(ForOfStmt *stmt) {
  pushScope(); // the hidden iterable
  const Type *iterableType = stmt->iterable->type;
  llvm::Value *iterableSlot = createManagedSlot(iterableType, "for.iterable");
  builder_.CreateStore(emitExpr(stmt->iterable), iterableSlot);

  llvm::BasicBlock *condBlock = newBlock("for.of.cond");
  llvm::BasicBlock *bodyBlock = newBlock("for.of.body");
  llvm::BasicBlock *endBlock = newBlock("for.of.end");
  emitArrayForOf(stmt, iterableSlot, condBlock, bodyBlock, endBlock);
  switchTo(endBlock);
  popScope();
}

void CodeGen::emitReturn(ReturnStmt *stmt) {
  if (stmt->value && tailGroup_ && hasTailCall(stmt->value)) {
    emitTailReturn(stmt->value);
    return;
  }
  emitReturnValue(stmt->value);
}

// `return value`, or `return` without one (null).
void CodeGen::emitReturnValue(Expr *expr) {
  llvm::Value *value = expr ? emitExpr(expr) : nullptr;
  if (value && returnType_ && returnType_->isVoid() && !expr->type->isVoid()) {
    // A callback whose result is dropped (see FuncSymbol::discardsResult).
    releaseValue(value, expr->type);
    value = nullptr;
  }
  emitCleanups(0);
  if (value && !expr->type->isVoid())
    builder_.CreateRet(value);
  else if (returnType_ && returnType_->isOptional())
    builder_.CreateRet(undefinedValue(returnType_)); // `return;`
  else
    builder_.CreateRetVoid();
  startDeadBlock();
}

void CodeGen::emitDiscarded(Expr *expr) {
  if (auto *assign = dyn_cast<AssignExpr>(expr)) {
    emitAssign(assign, /*needResult=*/false);
    return;
  }
  if (auto *seq = dyn_cast<SequenceExpr>(expr)) {
    for (Expr *e : seq->exprs)
      emitDiscarded(e);
    return;
  }
  llvm::Value *value = emitExpr(expr);
  if (expr->type->isManaged())
    releaseValue(value, expr->type);
}

//===----------------------------------------------------------------------===//
// Expressions
//===----------------------------------------------------------------------===//

llvm::Value *CodeGen::emitExpr(Expr *expr) {
  switch (expr->kind()) {
  case Expr::Kind::IntLit: {
    auto *lit = cast<IntLitExpr>(expr);
    if (lit->type->isFloat())
      return llvm::ConstantFP::get(builder_.getDoubleTy(), static_cast<double>(lit->value));
    return llvm::ConstantInt::get(lower(lit->type), lit->value);
  }
  case Expr::Kind::FloatLit:
    return llvm::ConstantFP::get(builder_.getDoubleTy(), cast<FloatLitExpr>(expr)->value);
  case Expr::Kind::BoolLit:
    return builder_.getInt1(cast<BoolLitExpr>(expr)->value);
  case Expr::Kind::StringLit:
    return stringLiteral(cast<StringLitExpr>(expr)->value);
  case Expr::Kind::Undefined:
    return builder_.getFalse(); // conversions and printing handle it
  case Expr::Kind::Template:
    return emitTemplate(cast<TemplateExpr>(expr));
  case Expr::Kind::ArrayLit:
    return emitArrayLiteral(cast<ArrayLitExpr>(expr));
  case Expr::Kind::ObjectLit:
    return emitObjectLiteral(cast<ObjectLitExpr>(expr));
  case Expr::Kind::Name: {
    auto *name = cast<NameExpr>(expr);
    if (name->func) // a top-level function as a value
      return makeFunctionValue(functionInfo(name->func->decl),
                               llvm::ConstantPointerNull::get(builder_.getPtrTy()));
    return emitVarValue(name->var, /*borrow=*/false);
  }
  case Expr::Kind::Function:
    return emitFunctionExpr(cast<FunctionExpr>(expr));
  case Expr::Kind::Member:
    return emitMember(cast<MemberExpr>(expr));
  case Expr::Kind::Index: {
    auto *index = cast<IndexExpr>(expr);
    if (index->enumDecl)
      return emitEnumName(index);
    if (index->object->type->isString())
      return emitStringIndex(index);
    return emitArrayIndex(index);
  }
  case Expr::Kind::Unary:
    return emitUnary(cast<UnaryExpr>(expr));
  case Expr::Kind::Binary:
    return emitBinary(cast<BinaryExpr>(expr));
  case Expr::Kind::Assign:
    return emitAssign(cast<AssignExpr>(expr), /*needResult=*/true);
  case Expr::Kind::Update:
    return emitUpdate(cast<UpdateExpr>(expr));
  case Expr::Kind::Call:
    return emitCall(cast<CallExpr>(expr));
  case Expr::Kind::New:
    return emitNewArray(cast<NewExpr>(expr));
  case Expr::Kind::Conditional:
    return emitConditional(cast<ConditionalExpr>(expr));
  case Expr::Kind::Cast: {
    auto *c = cast<CastExpr>(expr);
    if (c->shortCircuit)
      return emitChainLink(c);
    if (c->type->isFunction() && c->operand->type->isFunction())
      return emitFunctionAdapter(c);
    if (c->type->isBool() && !c->operand->type->isBool())
      return emitTruthOf(c->operand);
    return emitConversion(emitExpr(c->operand), c->operand->type, c->type);
  }
  case Expr::Kind::Sequence: {
    auto *seq = cast<SequenceExpr>(expr);
    for (size_t i = 0; i + 1 < seq->exprs.size(); ++i)
      emitDiscarded(seq->exprs[i]);
    return emitExpr(seq->exprs.back());
  }
  case Expr::Kind::OptionalChain:
    return emitOptionalChain(cast<OptionalChainExpr>(expr));
  }
  llvm_unreachable("unknown expression kind");
}

// `a?.b.c`: each link whose object is undefined jumps to the end, where the
// chain is undefined.
llvm::Value *CodeGen::emitOptionalChain(OptionalChainExpr *chain) {
  if (!chain->mayShortCircuit)
    return emitExpr(chain->chain);
  llvm::BasicBlock *undefinedBlock = newBlock("chain.undefined");
  llvm::BasicBlock *endBlock = newBlock("chain.end");
  chainEnds_.push_back(undefinedBlock);
  llvm::Value *value = emitExpr(chain->chain);
  chainEnds_.pop_back();
  const Type *type = chain->chain->type;
  if (!type->isVoid() && !type->isOptional())
    value = makeOptional(value, builder_.getTrue(), chain->type);
  llvm::BasicBlock *valueEnd = builder_.GetInsertBlock();
  builder_.CreateBr(endBlock);
  switchTo(undefinedBlock);
  builder_.CreateBr(endBlock);
  switchTo(endBlock);
  if (type->isVoid())
    return nullptr;
  llvm::PHINode *phi = builder_.CreatePHI(lower(chain->type), 2);
  phi->addIncoming(value, valueEnd);
  phi->addIncoming(undefinedValue(chain->type), undefinedBlock);
  return phi;
}

llvm::Value *CodeGen::emitChainLink(CastExpr *link) {
  const Type *type = link->operand->type;
  llvm::Value *value = emitExpr(link->operand);
  llvm::BasicBlock *valueBlock = newBlock("chain.link");
  // Undefined holds nothing to release.
  builder_.CreateCondBr(optionalHas(value, type), valueBlock, chainEnds_.back());
  switchTo(valueBlock);
  return optionalValue(value, type);
}

llvm::Value *CodeGen::emitUnary(UnaryExpr *unary) {
  if (unary->op == UnaryOp::TypeOf)
    return emitTypeOf(unary->operand);
  if (unary->op == UnaryOp::NonNull)
    return emitNonNull(unary);
  if (unary->op == UnaryOp::Void) {
    emitDiscarded(unary->operand);
    return builder_.getFalse(); // undefined, as for the literal
  }
  const Type *type = unary->operand->type;
  if ((unary->op == UnaryOp::Plus || unary->op == UnaryOp::Neg) && type->isString()) {
    // `+s` is Number(s), and `-s` is -Number(s).
    Operand str = emitOperand(unary->operand, /*canBorrow=*/true);
    llvm::Value *number = emitStringToNumber(str.value);
    dropOperand(str);
    return unary->op == UnaryOp::Neg ? builder_.CreateFNeg(number) : number;
  }
  llvm::Value *operand = emitExpr(unary->operand);
  switch (unary->op) {
  case UnaryOp::Neg:
    if (type->isBool()) // `-true` is -1
      return builder_.CreateFNeg(builder_.CreateUIToFP(operand, builder_.getDoubleTy()));
    return unary->type->isFloat() ? builder_.CreateFNeg(operand) : builder_.CreateNeg(operand);
  case UnaryOp::Plus:
    return type->isBool() ? builder_.CreateUIToFP(operand, builder_.getDoubleTy()) : operand;
  case UnaryOp::Not:
  case UnaryOp::BitNot:
    return builder_.CreateNot(operand);
  case UnaryOp::TypeOf:
  case UnaryOp::NonNull:
  case UnaryOp::Void:
  case UnaryOp::Delete: // Sema rejects it for now
    break;
  }
  llvm_unreachable("unknown unary operator");
}

// `x!`: the program stops if x is undefined.
llvm::Value *CodeGen::emitNonNull(UnaryExpr *unary) {
  const Type *type = unary->operand->type;
  llvm::Value *value = emitExpr(unary->operand);
  if (!type->isOptional())
    return value;
  llvm::BasicBlock *panicBlock = newBlock("nonnull.undefined");
  llvm::BasicBlock *okBlock = newBlock("nonnull.ok");
  builder_.CreateCondBr(builder_.CreateNot(optionalHas(value, type)), panicBlock, okBlock,
                        llvm::MDBuilder(ctx_).createUnlikelyBranchWeights());
  switchTo(panicBlock);
  emitPanic("TypeError: the value asserted with '!' is undefined");
  switchTo(okBlock);
  return optionalValue(value, type);
}

static std::u16string ascii16(llvm::StringRef text) { return std::u16string(text.begin(), text.end()); }

// `typeof x` follows from the type of x, except for `T | undefined`.
llvm::Value *CodeGen::emitTypeOf(Expr *operand) {
  const Type *type = operand->type;
  if (type->isOptional()) {
    Operand value = emitOperand(operand, /*canBorrow=*/true);
    llvm::Value *has = optionalHas(value.value, type);
    dropOperand(value);
    return builder_.CreateSelect(has, stringLiteral(ascii16(typeOfName(type->inner()))),
                                 stringLiteral(u"undefined"));
  }
  if (!isa<NameExpr>(operand)) // evaluated for its side effects
    emitDiscarded(operand);
  return stringLiteral(ascii16(typeOfName(type)));
}

// `Color[value]`: the name of the enum member with that value, or undefined.
llvm::Value *CodeGen::emitEnumName(IndexExpr *index) {
  llvm::Value *value = emitExpr(index->index);
  const Type *type = index->type;
  llvm::Value *result = undefinedValue(type);
  // Each member's test wraps those of the members before it, so that of
  // members with one value the last names it, as in JavaScript.
  for (const EnumMember &m : index->enumDecl->members) {
    llvm::SmallVector<llvm::UTF16, 16> units;
    llvm::convertUTF8ToUTF16String(m.name, units);
    llvm::Value *name = makeOptional(stringLiteral(std::u16string(units.begin(), units.end())),
                                     builder_.getTrue(), type);
    llvm::Value *matches = builder_.CreateICmpEQ(value, builder_.getInt64(static_cast<uint64_t>(int64_t(m.number))));
    result = builder_.CreateSelect(matches, name, result);
  }
  return result;
}

llvm::Value *CodeGen::emitBinary(BinaryExpr *bin) {
  switch (bin->op) {
  case BinaryOp::LogAnd:
  case BinaryOp::LogOr:
    return emitLogical(bin);
  case BinaryOp::Nullish:
    return emitNullish(bin);
  case BinaryOp::Eq:
  case BinaryOp::Ne:
    return emitEquality(bin);
  default:
    break;
  }
  if (bin->type->isString())
    return emitConcat(bin);
  if (bin->lhs->type->isString())
    return emitStringComparison(bin);

  llvm::Value *lhs = emitExpr(bin->lhs);
  llvm::Value *rhs = emitExpr(bin->rhs);
  switch (bin->op) {
  case BinaryOp::Lt:
  case BinaryOp::Le:
  case BinaryOp::Gt:
  case BinaryOp::Ge:
    return emitComparison(bin->op, lhs, rhs, bin->lhs->type);
  case BinaryOp::UShr:
    // An int's `x >>> n` that Sema made a long: the unsigned 32-bit result.
    if (bin->type != bin->lhs->type)
      return builder_.CreateZExt(emitArithmetic(bin->op, lhs, rhs, bin->lhs->type), lower(bin->type));
    return emitArithmetic(bin->op, lhs, rhs, bin->type);
  default:
    return emitArithmetic(bin->op, lhs, rhs, bin->type);
  }
}

// `&&` and `||` evaluate their right side only when needed. When the left
// side decides, it is the result (Sema converted the right side to the type
// of the whole).
llvm::Value *CodeGen::emitLogical(BinaryExpr *bin) {
  bool isAnd = bin->op == BinaryOp::LogAnd;
  const Type *lhsType = bin->lhs->type;
  llvm::Value *lhs = emitExpr(bin->lhs);
  llvm::Value *truth = emitTruth(lhs, lhsType);
  llvm::Value *decided = decidedValue(bin, lhs); // used only when it decides
  llvm::BasicBlock *lhsEnd = builder_.GetInsertBlock();
  llvm::BasicBlock *rhsBlock = newBlock(isAnd ? "and.rhs" : "or.rhs");
  llvm::BasicBlock *endBlock = newBlock(isAnd ? "and.end" : "or.end");
  if (isAnd)
    builder_.CreateCondBr(truth, rhsBlock, endBlock);
  else
    builder_.CreateCondBr(truth, endBlock, rhsBlock);

  switchTo(rhsBlock);
  releaseValue(lhs, lhsType); // the right side gives the result
  llvm::Value *rhs = emitExpr(bin->rhs);
  llvm::BasicBlock *rhsEnd = builder_.GetInsertBlock();
  builder_.CreateBr(endBlock);

  switchTo(endBlock);
  llvm::PHINode *phi = builder_.CreatePHI(lower(bin->type), 2);
  phi->addIncoming(decided, lhsEnd);
  phi->addIncoming(rhs, rhsEnd);
  return phi;
}

llvm::Value *CodeGen::decidedValue(BinaryExpr *bin, llvm::Value *lhs) {
  bool isAnd = bin->op == BinaryOp::LogAnd;
  const Type *from = bin->lhs->type, *to = bin->type;
  if (from->isBool() && to->isBool())
    return builder_.getInt1(!isAnd);
  const Type *base = from->nonOptional();
  // A falsy array, function or object is undefined, which holds nothing to
  // release.
  if (isAnd && (base->isArray() || base->isFunction() || base->isObject()))
    return undefinedValue(to);
  if (from == to)
    return lhs;
  // A truthy `T | undefined` holds a value; the other side may be undefined.
  if (from->isOptional())
    return optionalValue(lhs, from);
  return makeOptional(lhs, builder_.getTrue(), to);
}

llvm::Value *CodeGen::emitTruth(llvm::Value *value, const Type *type) {
  switch (type->kind()) {
  case Type::Kind::Bool:
    return value;
  case Type::Kind::String:
    return builder_.CreateICmpNE(stringLength(value), builder_.getInt64(0));
  case Type::Kind::Optional: {
    llvm::Value *has = optionalHas(value, type);
    const Type *inner = type->inner();
    if (!inner->isBool() && !inner->isString())
      return has; // an array, a function or an object is always truthy
    return builder_.CreateAnd(has, emitTruth(optionalValue(value, type), inner));
  }
  default:
    llvm_unreachable("Sema rejects testing other types");
  }
}

llvm::Value *CodeGen::emitTruthOf(Expr *expr) {
  // A narrowed variable holds a value: its own truth is the same, and it can
  // be borrowed.
  if (auto *conv = dyn_cast<CastExpr>(expr);
      conv && conv->isImplicit() && !conv->shortCircuit && isa<NameExpr>(conv->operand) &&
      conv->operand->type->isOptional() && conv->type == conv->operand->type->inner())
    expr = conv->operand;
  Operand value = emitOperand(expr, /*canBorrow=*/true);
  llvm::Value *truth = emitTruth(value.value, value.type);
  dropOperand(value);
  return truth;
}

llvm::Value *CodeGen::emitValuesEqual(llvm::Value *a, llvm::Value *b, const Type *type) {
  switch (type->kind()) {
  case Type::Kind::String: {
    // Comparing with a literal needs no call when the literal is empty or
    // inline: every one-byte string that short is inline, so equal ones have
    // equal words.
    if (isa<llvm::Constant>(a))
      std::swap(a, b);
    auto *literal = dyn_cast<llvm::Constant>(b);
    if (auto *meta = literal ? llvm::dyn_cast_or_null<llvm::ConstantInt>(literal->getAggregateElement(1u))
                             : nullptr) {
      uint64_t bits = meta->getZExtValue();
      if ((bits & kStrLengthMask) == 0)
        return builder_.CreateICmpEQ(stringLength(a), builder_.getInt64(0));
      if ((bits & (kStrRope | kStrTwoByte | kStrHeap)) == kStrRope)
        return builder_.CreateAnd(builder_.CreateICmpEQ(stringObj(a), stringObj(b)),
                                  builder_.CreateICmpEQ(stringMeta(a), stringMeta(b)));
    }
    llvm::Value *args[] = {stringObj(a), stringMeta(a), stringObj(b), stringMeta(b)};
    return builder_.CreateICmpNE(builder_.CreateCall(rt_.strEqual, args), builder_.getInt32(0));
  }
  case Type::Kind::Array: // the same array
  case Type::Kind::Object: // the same object
  case Type::Kind::Bool:
    return builder_.CreateICmpEQ(a, b);
  case Type::Kind::Function: // the same function with the same environment
    return builder_.CreateAnd(
        builder_.CreateICmpEQ(builder_.CreateExtractValue(a, 0), builder_.CreateExtractValue(b, 0)),
        builder_.CreateICmpEQ(builder_.CreateExtractValue(a, 1), builder_.CreateExtractValue(b, 1)));
  case Type::Kind::Optional: {
    if (type->inner()->isObject()) // both null, or the same object
      return builder_.CreateICmpEQ(a, b);
    llvm::Value *hasA = builder_.CreateExtractValue(a, 1), *hasB = builder_.CreateExtractValue(b, 1);
    llvm::Value *values = emitValuesEqual(builder_.CreateExtractValue(a, 0),
                                          builder_.CreateExtractValue(b, 0), type->inner());
    // Both undefined, or both defined and equal. The zero values of undefined
    // are valid, so comparing them is harmless.
    llvm::Value *both = builder_.CreateAnd(hasA, hasB);
    llvm::Value *neither = builder_.CreateNot(builder_.CreateOr(hasA, hasB));
    return builder_.CreateOr(neither, builder_.CreateAnd(both, values));
  }
  default:
    return emitComparison(BinaryOp::Eq, a, b, type);
  }
}

// `==` and `!=`. Sema converted both sides to one type, unless one of them
// is `undefined`.
llvm::Value *CodeGen::emitEquality(BinaryExpr *bin) {
  bool isEq = bin->op == BinaryOp::Eq;
  Expr *lhs = bin->lhs, *rhs = bin->rhs;
  if (lhs->type->isUndefined() || rhs->type->isUndefined()) {
    // A side that is always undefined, such as `void f()`, still runs, in
    // its turn.
    if (lhs->type->isUndefined())
      emitDiscarded(lhs);
    Expr *other = lhs->type->isUndefined() ? rhs : lhs;
    if (other->type->isUndefined()) {
      emitDiscarded(rhs);
      return builder_.getInt1(isEq);
    }
    Operand value = emitOperand(other, /*canBorrow=*/other == rhs || cannotAssign(rhs));
    if (rhs->type->isUndefined())
      emitDiscarded(rhs);
    llvm::Value *has = other->type->isOptional() ? optionalHas(value.value, other->type) : builder_.getTrue();
    dropOperand(value);
    return isEq ? builder_.CreateNot(has) : has;
  }
  if (lhs->type->isString() && !lhs->type->isOptional())
    return emitStringComparison(bin);

  Operand a = emitOperand(lhs, cannotAssign(rhs));
  Operand b = emitOperand(rhs, /*canBorrow=*/true);
  llvm::Value *equal = emitValuesEqual(a.value, b.value, lhs->type);
  dropOperand(a);
  dropOperand(b);
  return isEq ? equal : builder_.CreateNot(equal);
}

// `a ?? b`: b is evaluated only when a is undefined.
llvm::Value *CodeGen::emitNullish(BinaryExpr *bin) {
  const Type *lhsType = bin->lhs->type;
  if (lhsType->isUndefined()) {
    emitDiscarded(bin->lhs); // `void f() ?? x` still runs f
    return emitExpr(bin->rhs);
  }
  if (!lhsType->isOptional()) // never undefined
    return emitExpr(bin->lhs);

  llvm::Value *lhs = emitExpr(bin->lhs);
  llvm::BasicBlock *valueBlock = newBlock("nullish.value");
  llvm::BasicBlock *defaultBlock = newBlock("nullish.default");
  llvm::BasicBlock *endBlock = newBlock("nullish.end");
  builder_.CreateCondBr(optionalHas(lhs, lhsType), valueBlock, defaultBlock);

  switchTo(valueBlock);
  llvm::Value *value = bin->type->isOptional() ? lhs : optionalValue(lhs, lhsType);
  llvm::BasicBlock *valueEnd = builder_.GetInsertBlock();
  builder_.CreateBr(endBlock);

  switchTo(defaultBlock); // lhs is undefined and holds nothing to release
  llvm::Value *fallback = emitExpr(bin->rhs);
  llvm::BasicBlock *defaultEnd = builder_.GetInsertBlock();
  builder_.CreateBr(endBlock);

  switchTo(endBlock);
  llvm::PHINode *phi = builder_.CreatePHI(lower(bin->type), 2);
  phi->addIncoming(value, valueEnd);
  phi->addIncoming(fallback, defaultEnd);
  return phi;
}

llvm::Value *CodeGen::emitArithmetic(BinaryOp op, llvm::Value *lhs, llvm::Value *rhs,
                                     const Type *type) {
  bool isFloat = type->isFloat();
  bool bitwise = op == BinaryOp::BitAnd || op == BinaryOp::BitOr || op == BinaryOp::BitXor ||
                 op == BinaryOp::Shl || op == BinaryOp::Shr || op == BinaryOp::UShr;
  if (isFloat && bitwise) {
    // `x |= v` where x is a double: on 32-bit integers, as in JavaScript (Sema
    // made v an int), and the result goes back as a double.
    llvm::Value *result =
        emitArithmetic(op, builder_.CreateCall(rt_.toInt32, {lhs}), rhs, Type::intTy());
    return builder_.CreateSIToFP(result, lhs->getType());
  }
  if (bitwise && lhs->getType() != rhs->getType()) {
    // `x |= v` where x is a long and v an int: on 32-bit integers too (Sema
    // left v an int), and the result goes back as a long.
    llvm::Value *result =
        emitArithmetic(op, builder_.CreateTrunc(lhs, rhs->getType()), rhs, Type::intTy());
    return builder_.CreateSExt(result, lhs->getType());
  }
  switch (op) {
  // Integer arithmetic wraps around on overflow, as in Go.
  case BinaryOp::Add:
    return isFloat ? builder_.CreateFAdd(lhs, rhs) : builder_.CreateAdd(lhs, rhs);
  case BinaryOp::Sub:
    return isFloat ? builder_.CreateFSub(lhs, rhs) : builder_.CreateSub(lhs, rhs);
  case BinaryOp::Mul:
    return isFloat ? builder_.CreateFMul(lhs, rhs) : builder_.CreateMul(lhs, rhs);
  case BinaryOp::Div:
    return isFloat ? builder_.CreateFDiv(lhs, rhs) : emitIntDivRem(op, lhs, rhs);
  case BinaryOp::Rem:
    return isFloat ? builder_.CreateFRem(lhs, rhs) : emitIntDivRem(op, lhs, rhs);
  case BinaryOp::Pow: {
    if (isFloat)
      return builder_.CreateCall(rt_.mathPow, {lhs, rhs});
    // Integers: computed as longs, which wrap around like repeated `*`; the
    // low half of that is the int result.
    llvm::Type *i64 = builder_.getInt64Ty();
    llvm::Value *result = builder_.CreateCall(
        rt_.mathPowLong, {builder_.CreateSExt(lhs, i64), builder_.CreateSExt(rhs, i64)});
    return builder_.CreateTrunc(result, lhs->getType());
  }
  case BinaryOp::BitAnd:
    return builder_.CreateAnd(lhs, rhs);
  case BinaryOp::BitOr:
    return builder_.CreateOr(lhs, rhs);
  case BinaryOp::BitXor:
    return builder_.CreateXor(lhs, rhs);
  case BinaryOp::Shl:
  case BinaryOp::Shr:
  case BinaryOp::UShr: {
    // As in JavaScript, only the low bits of the count are used. This also
    // keeps the shift defined for any count.
    llvm::Value *mask = llvm::ConstantInt::get(lhs->getType(), type->bitWidth() - 1);
    llvm::Value *count = builder_.CreateAnd(rhs, mask);
    if (op == BinaryOp::Shl)
      return builder_.CreateShl(lhs, count);
    if (op == BinaryOp::Shr)
      return builder_.CreateAShr(lhs, count);
    return builder_.CreateLShr(lhs, count);
  }
  default:
    break;
  }
  llvm_unreachable("not an arithmetic operator");
}

llvm::Value *CodeGen::emitComparison(BinaryOp op, llvm::Value *lhs, llvm::Value *rhs,
                                     const Type *type) {
  if (type->isFloat()) {
    switch (op) {
    case BinaryOp::Lt: return builder_.CreateFCmpOLT(lhs, rhs);
    case BinaryOp::Le: return builder_.CreateFCmpOLE(lhs, rhs);
    case BinaryOp::Gt: return builder_.CreateFCmpOGT(lhs, rhs);
    case BinaryOp::Ge: return builder_.CreateFCmpOGE(lhs, rhs);
    case BinaryOp::Eq: return builder_.CreateFCmpOEQ(lhs, rhs);
    case BinaryOp::Ne: return builder_.CreateFCmpUNE(lhs, rhs); // NaN != NaN
    default: break;
    }
  } else {
    switch (op) {
    case BinaryOp::Lt: return builder_.CreateICmpSLT(lhs, rhs);
    case BinaryOp::Le: return builder_.CreateICmpSLE(lhs, rhs);
    case BinaryOp::Gt: return builder_.CreateICmpSGT(lhs, rhs);
    case BinaryOp::Ge: return builder_.CreateICmpSGE(lhs, rhs);
    case BinaryOp::Eq: return builder_.CreateICmpEQ(lhs, rhs);
    case BinaryOp::Ne: return builder_.CreateICmpNE(lhs, rhs);
    default: break;
    }
  }
  llvm_unreachable("not a comparison operator");
}

// Dividing by zero panics, as in Go. MIN / -1 wraps around to MIN (and
// MIN % -1 is 0) where the machine instruction would trap.
llvm::Value *CodeGen::emitIntDivRem(BinaryOp op, llvm::Value *lhs, llvm::Value *rhs) {
  bool isRem = op == BinaryOp::Rem;
  if (auto *divisor = dyn_cast<llvm::ConstantInt>(rhs);
      divisor && !divisor->isZero() && !divisor->isMinusOne())
    return isRem ? builder_.CreateSRem(lhs, rhs) : builder_.CreateSDiv(lhs, rhs);

  llvm::Type *type = lhs->getType();
  llvm::Value *isZero = builder_.CreateICmpEQ(rhs, llvm::ConstantInt::get(type, 0));
  llvm::BasicBlock *panicBlock = newBlock("div.zero");
  llvm::BasicBlock *okBlock = newBlock("div.ok");
  builder_.CreateCondBr(isZero, panicBlock, okBlock,
                        llvm::MDBuilder(ctx_).createUnlikelyBranchWeights());
  switchTo(panicBlock);
  emitPanic("integer divide by zero");

  switchTo(okBlock);
  llvm::Value *isMinusOne = builder_.CreateICmpEQ(rhs, llvm::ConstantInt::getSigned(type, -1));
  llvm::Value *divisor = builder_.CreateSelect(isMinusOne, llvm::ConstantInt::get(type, 1), rhs);
  if (isRem)
    return builder_.CreateSRem(lhs, divisor); // x % 1 == x % -1 == 0
  llvm::Value *quotient = builder_.CreateSDiv(lhs, divisor);
  return builder_.CreateSelect(isMinusOne, builder_.CreateNeg(lhs), quotient);
}

void CodeGen::emitPanic(llvm::StringRef message) {
  llvm::CallInst *call =
      builder_.CreateCall(rt_.panic, {utf8Constant(message), builder_.getInt64(message.size())});
  call->setDoesNotReturn();
  builder_.CreateUnreachable();
}

llvm::Value *CodeGen::emitAssign(AssignExpr *assign, bool needResult) {
  if (assign->op && *assign->op == BinaryOp::Nullish)
    return emitNullishAssign(assign, needResult);
  if (isa<IndexExpr>(assign->target))
    return emitElementAssign(assign, needResult);
  if (auto *member = dyn_cast<MemberExpr>(assign->target))
    return member->object->type->isObject() ? emitFieldAssign(assign, needResult)
                                            : emitLengthAssign(assign, needResult);

  const VarSymbol *var = cast<NameExpr>(assign->target)->var;
  llvm::Value *address = addressOf(var);
  const Type *type = assign->type;
  llvm::Value *value;
  if (!assign->op) {
    value = emitExpr(assign->value);
  } else if (type->isString()) {
    // `s += x`: the variable's own value is borrowed unless evaluating `x`
    // could reassign the variable.
    Operand old{builder_.CreateLoad(strTy_, address), false, type};
    if (!cannotAssign(assign->value)) {
      retainString(old.value);
      old.owned = true;
    }
    Operand rhs = emitConcatPart(assign->value, /*canBorrow=*/false);
    value = concatOperands({old, rhs});
  } else {
    // As in JavaScript, the old value is read before the right side runs.
    llvm::Value *old = builder_.CreateLoad(lower(type), address);
    llvm::Value *rhs = emitExpr(assign->value);
    value = emitArithmetic(*assign->op, old, rhs, type);
  }
  storeReplacing(address, value, type);
  if (!needResult)
    return nullptr;
  retainValue(value, type);
  return value;
}

// `x ??= v`
llvm::Value *CodeGen::emitNullishAssign(AssignExpr *assign, bool needResult) {
  const VarSymbol *var = cast<NameExpr>(assign->target)->var;
  const Type *varType = var->type;
  llvm::Value *address = addressOf(var);
  llvm::Value *old = builder_.CreateLoad(lower(varType), address);
  llvm::BasicBlock *keepBlock = newBlock("nullish.keep");
  llvm::BasicBlock *assignBlock = newBlock("nullish.assign");
  llvm::BasicBlock *endBlock = newBlock("nullish.end");
  builder_.CreateCondBr(optionalHas(old, varType), keepBlock, assignBlock);

  const Type *resultType = assign->type;
  switchTo(keepBlock);
  llvm::Value *kept = nullptr;
  if (needResult) {
    retainValue(old, varType);
    kept = resultType->isOptional() ? old : optionalValue(old, varType);
  }
  llvm::BasicBlock *keepEnd = builder_.GetInsertBlock();
  builder_.CreateBr(endBlock);

  switchTo(assignBlock);
  llvm::Value *value = emitExpr(assign->value);
  const Type *valueType = assign->value->type;
  llvm::Value *stored = valueType->isOptional() ? value : makeOptional(value, builder_.getTrue(), varType);
  storeReplacing(address, stored, varType);
  llvm::Value *assigned = nullptr;
  if (needResult) {
    retainValue(stored, varType);
    assigned = resultType->isOptional() ? stored : optionalValue(stored, varType);
  }
  llvm::BasicBlock *assignEnd = builder_.GetInsertBlock();
  builder_.CreateBr(endBlock);

  switchTo(endBlock);
  if (!needResult)
    return nullptr;
  llvm::PHINode *phi = builder_.CreatePHI(lower(resultType), 2);
  phi->addIncoming(kept, keepEnd);
  phi->addIncoming(assigned, assignEnd);
  return phi;
}

llvm::Value *CodeGen::emitUpdate(UpdateExpr *update) {
  llvm::Value *address;
  Operand holder{nullptr, false, nullptr};
  if (auto *index = dyn_cast<IndexExpr>(update->target)) {
    // `a[i]++`: Sema proved the index in range.
    holder = emitOperand(index->object, cannotAssign(index->index));
    llvm::Value *position = emitExpr(index->index);
    address = elementAddress(arrayData(holder.value), update->type, position);
  } else if (auto *member = dyn_cast<MemberExpr>(update->target)) {
    // `o.count++`: the object holds the number.
    holder = emitOperand(member->object, /*canBorrow=*/true);
    address = fieldAddress(holder.value, member->object->type, member->name);
  } else {
    address = addressOf(cast<NameExpr>(update->target)->var);
  }
  llvm::Type *type = lower(update->type);
  llvm::Value *old = builder_.CreateLoad(type, address);
  llvm::Value *updated;
  if (update->type->isFloat()) {
    llvm::Value *one = llvm::ConstantFP::get(type, 1.0);
    updated = update->increment ? builder_.CreateFAdd(old, one) : builder_.CreateFSub(old, one);
  } else {
    llvm::Value *one = llvm::ConstantInt::get(type, 1);
    updated = update->increment ? builder_.CreateAdd(old, one) : builder_.CreateSub(old, one);
  }
  builder_.CreateStore(updated, address);
  if (holder.value)
    dropOperand(holder);
  return update->prefix ? updated : old;
}

llvm::Value *CodeGen::emitCall(CallExpr *call) {
  switch (call->builtin) {
  case Builtin::ConsoleLog:
    emitConsoleLog(call);
    return nullptr;
  case Builtin::StringOf:
    // Sema already converted the argument to a string.
    return call->args.empty() ? stringLiteral(u"") : emitExpr(call->args[0]);
  case Builtin::NumberOf:
  case Builtin::ParseInt:
  case Builtin::ParseFloat:
  case Builtin::IsNaN:
  case Builtin::IsFinite:
  case Builtin::IsInteger:
  case Builtin::IsSafeInteger:
  case Builtin::FromCharCode:
  case Builtin::FromCodePoint:
  case Builtin::NumToFixed:
    return emitNumberBuiltin(call);
  case Builtin::None:
    break;
  case Builtin::ArrForEach:
  case Builtin::ArrMap:
  case Builtin::ArrFilter:
  case Builtin::ArrFlatMap:
  case Builtin::ArrReduce:
  case Builtin::ArrReduceRight:
  case Builtin::ArrFind:
  case Builtin::ArrFindIndex:
  case Builtin::ArrFindLast:
  case Builtin::ArrFindLastIndex:
  case Builtin::ArrSome:
  case Builtin::ArrEvery:
  case Builtin::ArrSortWith:
    return emitCallbackMethod(call);
  default:
    if (isMathBuiltin(call->builtin))
      return emitMathCall(call);
    if (cast<MemberExpr>(call->callee)->object->type->isArray())
      return emitArrayMethod(call);
    return emitStringMethod(call);
  }

  if (!call->func)
    return emitValueCall(call);
  // The callee takes ownership of its arguments. Parameters left out get
  // undefined (and then their default values).
  const FuncSymbol *func = call->func;
  std::vector<llvm::Value *> args;
  for (Expr *arg : call->args)
    args.push_back(emitExpr(arg));
  for (size_t i = args.size(); i < func->params.size(); ++i)
    args.push_back(undefinedValue(func->params[i]));
  if (func->decl->isClosure()) {
    // A function declared in a block, or a constant arrow: its code is
    // known, its environment comes from where it is called.
    args.insert(args.begin(), closureEnv(call));
    llvm::CallInst *result = builder_.CreateCall(closureCode(func->decl), args);
    return result->getType()->isVoidTy() ? nullptr : result;
  }
  llvm::CallInst *result = builder_.CreateCall(functions_.lookup(func), args);
  return result->getType()->isVoidTy() ? nullptr : result;
}

// console.log(a, b) prints its arguments separated by spaces, then a newline.
// Strings print as they are; arrays and optionals the way Node.js shows them.
// As in JavaScript, all arguments are evaluated before anything is printed:
// `console.log(a.pop(), a)` shows the array without the popped element.
void CodeGen::emitConsoleLog(CallExpr *call) {
  size_t count = call->args.size();
  std::vector<bool> laterCannotAssign(count, true);
  for (size_t i = count; i > 1; --i)
    laterCannotAssign[i - 2] = laterCannotAssign[i - 1] && cannotAssign(call->args[i - 1]);
  std::vector<Operand> values;
  for (size_t i = 0; i < count; ++i) {
    if (call->args[i]->type->isUndefined()) {
      emitDiscarded(call->args[i]); // `void f()` still runs f
      values.push_back({nullptr, false, call->args[i]->type});
    } else
      values.push_back(emitOperand(call->args[i], laterCannotAssign[i]));
  }

  for (size_t i = 0; i < count; ++i) {
    if (i > 0)
      builder_.CreateCall(rt_.printChar, {builder_.getInt32(' ')});
    const Operand &value = values[i];
    const Type *type = value.type;
    if (type->isUndefined()) {
      llvm::Value *text = stringLiteral(u"undefined");
      builder_.CreateCall(rt_.printString, {stringObj(text), stringMeta(text)});
      continue;
    }
    switch (type->kind()) {
    case Type::Kind::String:
      builder_.CreateCall(rt_.printString, {stringObj(value.value), stringMeta(value.value)});
      break;
    case Type::Kind::Int:
      builder_.CreateCall(rt_.printI32, {value.value});
      break;
    case Type::Kind::Long:
      builder_.CreateCall(rt_.printI64, {value.value});
      break;
    case Type::Kind::Double:
      builder_.CreateCall(rt_.printF64, {value.value});
      break;
    case Type::Kind::Bool:
      builder_.CreateCall(rt_.printBool, {builder_.CreateZExt(value.value, builder_.getInt32Ty())});
      break;
    case Type::Kind::Array:
    case Type::Kind::Optional:
    case Type::Kind::Function:
    case Type::Kind::Object:
      builder_.CreateCall(rt_.printValue, {typeDescriptor(type), spill(value.value, "print.value")});
      break;
    default:
      llvm_unreachable("Sema rejects other argument types");
    }
    dropOperand(value);
  }
  builder_.CreateCall(rt_.printChar, {builder_.getInt32('\n')});
}

llvm::Value *CodeGen::emitConditional(ConditionalExpr *cond) {
  llvm::Value *test = emitExpr(cond->cond);
  llvm::BasicBlock *thenBlock = newBlock("cond.then");
  llvm::BasicBlock *elseBlock = newBlock("cond.else");
  llvm::BasicBlock *endBlock = newBlock("cond.end");
  builder_.CreateCondBr(test, thenBlock, elseBlock);

  switchTo(thenBlock);
  llvm::Value *thenValue = emitExpr(cond->thenExpr);
  llvm::BasicBlock *thenEnd = builder_.GetInsertBlock();
  builder_.CreateBr(endBlock);

  switchTo(elseBlock);
  llvm::Value *elseValue = emitExpr(cond->elseExpr);
  llvm::BasicBlock *elseEnd = builder_.GetInsertBlock();
  builder_.CreateBr(endBlock);

  switchTo(endBlock);
  if (cond->type->isVoid())
    return nullptr;
  llvm::PHINode *phi = builder_.CreatePHI(lower(cond->type), 2);
  phi->addIncoming(thenValue, thenEnd);
  phi->addIncoming(elseValue, elseEnd);
  return phi;
}

llvm::Value *CodeGen::emitConversion(llvm::Value *value, const Type *from, const Type *to) {
  if (from == to)
    return value;

  if (to->isString()) {
    if (from->isBool())
      return builder_.CreateSelect(value, stringLiteral(u"true"), stringLiteral(u"false"));
    if (from->isInteger())
      return callReturningString(rt_.strFromI64,
                                 {builder_.CreateSExt(value, builder_.getInt64Ty())});
    if (from->isFloat())
      return callReturningString(rt_.strFromF64, {value});
    if (from->isUndefined())
      return stringLiteral(u"undefined");
    // Arrays and optionals: the runtime knows how to show any value.
    llvm::Value *result = emitValueToString(value, from);
    releaseValue(value, from);
    return result;
  }

  if (to->isOptional()) {
    if (from->isUndefined())
      return undefinedValue(to);
    if (from->isOptional()) // numeric widening, which keeps zero zero, or an object
      return makeOptional(emitConversion(optionalValue(value, from), from->inner(), to->inner()),
                          optionalHas(value, from), to);
    return makeOptional(emitConversion(value, from, to->inner()), builder_.getTrue(), to);
  }
  if (from->isOptional()) // narrowed: known to hold a value
    return emitConversion(optionalValue(value, from), from->inner(), to);
  if (from->isObject()) // seen through a type with read-only fields
    return value;

  llvm::Type *target = lower(to);
  if (from->isInteger() && to->isInteger())
    return from->bitWidth() < to->bitWidth() ? builder_.CreateSExt(value, target)
                                             : builder_.CreateTrunc(value, target);
  if (from->isInteger() && to->isFloat())
    return builder_.CreateSIToFP(value, target);
  // An operand of a bitwise operator: JavaScript's ToInt32. A double becomes a
  // long only through Math functions (emitMathCall).
  if (from->isFloat() && to->isInt())
    return builder_.CreateCall(rt_.toInt32, {value});
  llvm_unreachable("Sema rejects this conversion");
}

//===----------------------------------------------------------------------===//
// Strings
//===----------------------------------------------------------------------===//

llvm::Value *CodeGen::stringObj(llvm::Value *str) { return builder_.CreateExtractValue(str, 0); }

llvm::Value *CodeGen::stringMeta(llvm::Value *str) { return builder_.CreateExtractValue(str, 1); }

llvm::Value *CodeGen::stringLength(llvm::Value *str) {
  return builder_.CreateAnd(stringMeta(str), builder_.getInt64(kStrLengthMask), "length");
}

bool CodeGen::cannotAssign(const Expr *expr) {
  switch (expr->kind()) {
  case Expr::Kind::IntLit:
  case Expr::Kind::FloatLit:
  case Expr::Kind::BoolLit:
  case Expr::Kind::StringLit:
  case Expr::Kind::Undefined:
  case Expr::Kind::Name:
  case Expr::Kind::Function: // creating a closure runs none of its code
    return true;
  case Expr::Kind::Template:
    return llvm::all_of(cast<TemplateExpr>(expr)->exprs, cannotAssign);
  case Expr::Kind::ArrayLit:
    return llvm::all_of(cast<ArrayLitExpr>(expr)->elements,
                        [](const ArrayLitExpr::Element &e) { return cannotAssign(e.expr); });
  case Expr::Kind::ObjectLit:
    return llvm::all_of(cast<ObjectLitExpr>(expr)->props,
                        [](const ObjectLitExpr::Property &p) { return cannotAssign(p.value); });
  case Expr::Kind::Member:
    return cannotAssign(cast<MemberExpr>(expr)->object);
  case Expr::Kind::Index: {
    auto *index = cast<IndexExpr>(expr);
    return cannotAssign(index->object) && cannotAssign(index->index);
  }
  case Expr::Kind::Unary:
    return cannotAssign(cast<UnaryExpr>(expr)->operand);
  case Expr::Kind::Binary: {
    auto *bin = cast<BinaryExpr>(expr);
    return cannotAssign(bin->lhs) && cannotAssign(bin->rhs);
  }
  case Expr::Kind::Conditional: {
    auto *cond = cast<ConditionalExpr>(expr);
    return cannotAssign(cond->cond) && cannotAssign(cond->thenExpr) &&
           cannotAssign(cond->elseExpr);
  }
  case Expr::Kind::Cast:
    return cannotAssign(cast<CastExpr>(expr)->operand);
  case Expr::Kind::Sequence:
    return llvm::all_of(cast<SequenceExpr>(expr)->exprs, cannotAssign);
  case Expr::Kind::OptionalChain:
    return cannotAssign(cast<OptionalChainExpr>(expr)->chain);
  case Expr::Kind::New:
    return llvm::all_of(cast<NewExpr>(expr)->args, cannotAssign);
  case Expr::Kind::Call: {
    // A user function may assign to globals and fields, and so may the
    // callback of a built-in; other built-ins only evaluate their operands
    // (array methods change arrays, not variables).
    auto *call = cast<CallExpr>(expr);
    if (call->builtin == Builtin::None ||
        (call->builtin >= Builtin::ArrForEach && call->builtin <= Builtin::ArrSortWith))
      return false;
    if (!llvm::all_of(call->args, cannotAssign))
      return false;
    auto *member = dyn_cast<MemberExpr>(call->callee);
    return !member || call->builtin == Builtin::ConsoleLog || isMathBuiltin(call->builtin) ||
           cannotAssign(member->object);
  }
  case Expr::Kind::Assign:
  case Expr::Kind::Update:
    return false;
  }
  return false;
}

llvm::Value *CodeGen::callReturningString(llvm::FunctionCallee fn,
                                          llvm::ArrayRef<llvm::Value *> args) {
  llvm::AllocaInst *out = createStackSlot(strTy_, "str.out");
  std::vector<llvm::Value *> all{out};
  all.insert(all.end(), args.begin(), args.end());
  builder_.CreateCall(fn, all);
  return builder_.CreateLoad(strTy_, out);
}

llvm::Value *CodeGen::concatOperands(llvm::ArrayRef<Operand> operands) {
  auto *arrayTy = llvm::ArrayType::get(strTy_, operands.size());
  llvm::AllocaInst *array = createStackSlot(arrayTy, "concat.parts");
  for (size_t i = 0; i < operands.size(); ++i)
    builder_.CreateStore(operands[i].value,
                         builder_.CreateConstInBoundsGEP2_32(arrayTy, array, 0, static_cast<unsigned>(i)));
  llvm::Value *result =
      callReturningString(rt_.strConcatN, {array, builder_.getInt64(operands.size())});
  for (const Operand &operand : operands)
    dropOperand(operand);
  return result;
}

llvm::Value *CodeGen::emitConcat(BinaryExpr *bin) {
  std::vector<Expr *> parts;
  collectConcat(bin, parts);
  // A part can be borrowed if nothing after it can reassign its variable.
  std::vector<bool> laterCannotAssign(parts.size(), true);
  for (size_t i = parts.size() - 1; i > 0; --i)
    laterCannotAssign[i - 1] = laterCannotAssign[i] && cannotAssign(parts[i]);

  std::vector<Operand> operands;
  for (size_t i = 0; i < parts.size(); ++i)
    operands.push_back(emitConcatPart(parts[i], laterCannotAssign[i]));
  return concatOperands(operands);
}

// A part of a concatenation. A number converted to a string goes to the
// runtime as it is, which writes its digits straight into the result instead
// of making a string for them first.
CodeGen::Operand CodeGen::emitConcatPart(Expr *expr, bool canBorrow) {
  auto *conv = dyn_cast<CastExpr>(expr);
  const Type *from = conv && conv->isImplicit() ? conv->operand->type : nullptr;
  if (!from || !(from->isInteger() || from->isFloat()))
    return emitOperand(expr, canBorrow);
  llvm::Value *value = emitExpr(conv->operand);
  llvm::Value *bits = from->isFloat() ? builder_.CreateBitCast(value, builder_.getInt64Ty())
                                      : builder_.CreateSExt(value, builder_.getInt64Ty());
  llvm::Value *part = llvm::UndefValue::get(strTy_);
  part = builder_.CreateInsertValue(part, builder_.CreateIntToPtr(bits, builder_.getPtrTy()), 0);
  part = builder_.CreateInsertValue(
      part, builder_.getInt64(kNumberPart | (from->isFloat() ? kNumberDouble : kNumberInt)), 1);
  return {part, /*owned=*/false, Type::string()};
}

llvm::Value *CodeGen::emitTemplate(TemplateExpr *tmpl) {
  size_t count = tmpl->exprs.size();
  std::vector<bool> laterCannotAssign(count, true);
  for (size_t i = count; i > 1; --i)
    laterCannotAssign[i - 2] = laterCannotAssign[i - 1] && cannotAssign(tmpl->exprs[i - 1]);

  // `${x}` alone is just x as a string.
  if (count == 1 && tmpl->quasis[0].empty() && tmpl->quasis[1].empty())
    return emitExpr(tmpl->exprs[0]);
  std::vector<Operand> operands;
  for (size_t i = 0; i <= count; ++i) {
    if (!tmpl->quasis[i].empty())
      operands.push_back({stringLiteral(tmpl->quasis[i]), false, Type::string()});
    if (i < count)
      operands.push_back(emitConcatPart(tmpl->exprs[i], laterCannotAssign[i]));
  }
  if (operands.empty())
    return stringLiteral(u"");
  if (operands.size() == 1) // a template without substitutions
    return operands[0].value;
  return concatOperands(operands);
}

llvm::Value *CodeGen::emitStringComparison(BinaryExpr *bin) {
  Operand lhs = emitOperand(bin->lhs, cannotAssign(bin->rhs));
  Operand rhs = emitOperand(bin->rhs, /*canBorrow=*/true);
  llvm::Value *args[] = {stringObj(lhs.value), stringMeta(lhs.value), stringObj(rhs.value),
                         stringMeta(rhs.value)};
  llvm::Value *zero = builder_.getInt32(0);
  llvm::Value *result;
  switch (bin->op) {
  case BinaryOp::Eq:
    result = emitValuesEqual(lhs.value, rhs.value, Type::string());
    break;
  case BinaryOp::Ne:
    result = builder_.CreateNot(emitValuesEqual(lhs.value, rhs.value, Type::string()));
    break;
  case BinaryOp::Lt:
    result = builder_.CreateICmpSLT(builder_.CreateCall(rt_.strCompare, args), zero);
    break;
  case BinaryOp::Le:
    result = builder_.CreateICmpSLE(builder_.CreateCall(rt_.strCompare, args), zero);
    break;
  case BinaryOp::Gt:
    result = builder_.CreateICmpSGT(builder_.CreateCall(rt_.strCompare, args), zero);
    break;
  case BinaryOp::Ge:
    result = builder_.CreateICmpSGE(builder_.CreateCall(rt_.strCompare, args), zero);
    break;
  default:
    llvm_unreachable("not a string comparison");
  }
  dropOperand(lhs);
  dropOperand(rhs);
  return result;
}

llvm::Value *CodeGen::emitMember(MemberExpr *member) {
  if (member->object->type->isObject())
    return emitFieldRead(member);
  // Otherwise Sema allows only `.length` of a string or an array.
  Operand object = emitOperand(member->object, /*canBorrow=*/true);
  llvm::Value *length = object.type->isArray() ? arrayLength(object.value) : stringLength(object.value);
  dropOperand(object);
  return builder_.CreateTrunc(length, builder_.getInt32Ty(), "length");
}

// `s[i]`: a string of one code unit; `undefined` out of range unless Sema
// proved the index in range.
llvm::Value *CodeGen::emitStringIndex(IndexExpr *index) {
  Operand object = emitOperand(index->object, cannotAssign(index->index));
  llvm::Value *position = emitExpr(index->index);
  auto read = [&] {
    return callReturningString(rt_.strCharAt,
                               {stringObj(object.value), stringMeta(object.value), position});
  };
  llvm::Value *result;
  if (!index->mayBeOutOfRange)
    result = read();
  else
    result = emitCheckedRead(builder_.CreateICmpULT(position, stringLength(object.value)),
                             index->type, read);
  dropOperand(object);
  return result;
}

llvm::Value *CodeGen::emitStringMethod(CallExpr *call) {
  auto *member = cast<MemberExpr>(call->callee);
  if (call->builtin == Builtin::StrToString)
    return emitExpr(member->object);
  if (call->builtin == Builtin::StrConcat) {
    // `s.concat(a, b)` is `s + a + b`.
    std::vector<Expr *> parts{member->object};
    parts.insert(parts.end(), call->args.begin(), call->args.end());
    std::vector<Operand> operands;
    for (size_t i = 0; i < parts.size(); ++i) {
      bool later = std::all_of(parts.begin() + i + 1, parts.end(), cannotAssign);
      operands.push_back(emitConcatPart(parts[i], later));
    }
    return concatOperands(operands);
  }

  Operand receiver = emitOperand(member->object, llvm::all_of(call->args, cannotAssign));
  std::vector<Operand> strings;
  std::vector<llvm::Value *> numbers; // positions and counts, as i64
  for (size_t i = 0; i < call->args.size(); ++i) {
    Expr *arg = call->args[i];
    if (arg->type->isString()) {
      bool later = std::all_of(call->args.begin() + i + 1, call->args.end(), cannotAssign);
      strings.push_back(emitOperand(arg, later));
    } else {
      numbers.push_back(builder_.CreateSExt(emitExpr(arg), builder_.getInt64Ty()));
    }
  }
  // An optional position: its value and whether it was given.
  auto optional = [&](size_t i) -> std::pair<llvm::Value *, llvm::Value *> {
    if (i < numbers.size())
      return {numbers[i], builder_.getInt32(1)};
    return {builder_.getInt64(0), builder_.getInt32(0)};
  };
  // An optional string argument: its parts and whether it was given.
  auto optionalString = [&](size_t i) -> std::array<llvm::Value *, 3> {
    if (i < strings.size())
      return {stringObj(strings[i].value), stringMeta(strings[i].value), builder_.getInt32(1)};
    return {llvm::ConstantPointerNull::get(builder_.getPtrTy()), builder_.getInt64(0),
            builder_.getInt32(0)};
  };

  llvm::Value *obj = stringObj(receiver.value);
  llvm::Value *meta = stringMeta(receiver.value);
  llvm::Value *result = nullptr;
  switch (call->builtin) {
  case Builtin::StrCharCodeAt: {
    llvm::Value *position = numbers[0];
    if (!call->mayBeOutOfRange) {
      result = builder_.CreateCall(rt_.strCharCodeAt, {obj, meta, position});
      break;
    }
    // Out of range, JavaScript returns NaN.
    llvm::Value *inRange = builder_.CreateICmpULT(position, stringLength(receiver.value));
    llvm::BasicBlock *readBlock = newBlock("code.read");
    llvm::BasicBlock *endBlock = newBlock("code.end");
    llvm::BasicBlock *before = builder_.GetInsertBlock();
    builder_.CreateCondBr(inRange, readBlock, endBlock);
    switchTo(readBlock);
    llvm::Value *code = builder_.CreateSIToFP(builder_.CreateCall(rt_.strCharCodeAt, {obj, meta, position}),
                                              builder_.getDoubleTy());
    llvm::BasicBlock *readEnd = builder_.GetInsertBlock();
    builder_.CreateBr(endBlock);
    switchTo(endBlock);
    llvm::PHINode *phi = builder_.CreatePHI(builder_.getDoubleTy(), 2);
    phi->addIncoming(llvm::ConstantFP::getNaN(builder_.getDoubleTy()), before);
    phi->addIncoming(code, readEnd);
    result = phi;
    break;
  }
  case Builtin::StrAt: {
    // A negative position counts from the end.
    llvm::Value *length = stringLength(receiver.value);
    llvm::Value *position = numbers[0];
    position = builder_.CreateSelect(builder_.CreateICmpSLT(position, builder_.getInt64(0)),
                                     builder_.CreateAdd(position, length), position);
    auto read = [&] { return callReturningString(rt_.strCharAt, {obj, meta, position}); };
    result = call->mayBeOutOfRange
                 ? emitCheckedRead(builder_.CreateICmpULT(position, length), call->type, read)
                 : read();
    break;
  }
  case Builtin::StrSlice:
  case Builtin::StrSubstring: {
    llvm::Value *start = numbers.empty() ? builder_.getInt64(0) : numbers[0];
    auto [end, hasEnd] = optional(1);
    result = callReturningString(
        call->builtin == Builtin::StrSlice ? rt_.strSlice : rt_.strSubstring,
        {obj, meta, start, end, hasEnd});
    break;
  }
  case Builtin::StrIndexOf:
  case Builtin::StrIncludes:
  case Builtin::StrStartsWith:
  case Builtin::StrEndsWith: {
    llvm::FunctionCallee fn = call->builtin == Builtin::StrIndexOf    ? rt_.strIndexOf
                              : call->builtin == Builtin::StrIncludes ? rt_.strIncludes
                              : call->builtin == Builtin::StrStartsWith ? rt_.strStartsWith
                                                                        : rt_.strEndsWith;
    llvm::Value *search = strings[0].value;
    auto [position, hasPosition] = optional(0);
    result = builder_.CreateCall(
        fn, {obj, meta, stringObj(search), stringMeta(search), position, hasPosition});
    if (call->builtin != Builtin::StrIndexOf)
      result = builder_.CreateICmpNE(result, builder_.getInt32(0));
    break;
  }
  case Builtin::StrRepeat:
    result = callReturningString(rt_.strRepeat, {obj, meta, numbers[0]});
    break;
  case Builtin::StrCharAt: // "" out of range
    result = callReturningString(rt_.strCharAtOrEmpty,
                                 {obj, meta, numbers.empty() ? builder_.getInt64(0) : numbers[0]});
    break;
  case Builtin::StrCodePointAt: {
    llvm::Value *position = numbers[0];
    auto read = [&]() -> llvm::Value * {
      return builder_.CreateCall(rt_.strCodePoint, {obj, meta, position});
    };
    result = call->mayBeOutOfRange
                 ? emitCheckedRead(builder_.CreateICmpULT(position, stringLength(receiver.value)),
                                   call->type, read)
                 : read();
    break;
  }
  case Builtin::StrLastIndexOf: {
    llvm::Value *search = strings[0].value;
    auto [position, hasPosition] = optional(0);
    result = builder_.CreateCall(rt_.strLastIndexOf, {obj, meta, stringObj(search), stringMeta(search),
                                                      position, hasPosition});
    break;
  }
  case Builtin::StrSplit: {
    auto [sepObj, sepMeta, hasSep] = optionalString(0);
    auto [limit, hasLimit] = optional(0);
    result = builder_.CreateCall(rt_.strSplit, {typeDescriptor(Type::string()), obj, meta, sepObj,
                                                sepMeta, hasSep, limit, hasLimit},
                                 "split");
    break;
  }
  case Builtin::StrTrim:
  case Builtin::StrTrimStart:
  case Builtin::StrTrimEnd: {
    int ends = call->builtin == Builtin::StrTrimStart ? 1 : call->builtin == Builtin::StrTrimEnd ? 2 : 3;
    result = callReturningString(rt_.strTrim, {obj, meta, builder_.getInt32(ends)});
    break;
  }
  case Builtin::StrPadStart:
  case Builtin::StrPadEnd: {
    auto [fillObj, fillMeta, hasFill] = optionalString(0);
    result = callReturningString(rt_.strPad, {obj, meta, numbers[0], fillObj, fillMeta, hasFill,
                                              builder_.getInt32(call->builtin == Builtin::StrPadStart)});
    break;
  }
  case Builtin::StrToUpperCase:
  case Builtin::StrToLowerCase:
    result = callReturningString(
        call->builtin == Builtin::StrToUpperCase ? rt_.strToUpper : rt_.strToLower, {obj, meta});
    break;
  case Builtin::StrReplace:
  case Builtin::StrReplaceAll: {
    llvm::Value *pattern = strings[0].value, *replacement = strings[1].value;
    result = callReturningString(
        rt_.strReplace, {obj, meta, stringObj(pattern), stringMeta(pattern), stringObj(replacement),
                         stringMeta(replacement), builder_.getInt32(call->builtin == Builtin::StrReplaceAll)});
    break;
  }
  default:
    llvm_unreachable("not a string method");
  }

  dropOperand(receiver);
  for (const Operand &str : strings)
    dropOperand(str);
  return result;
}

// A short literal is an inline string. A longer one lives in the executable
// in the same layout as a heap buffer (see runtime/string.c), one byte per
// code unit when all of them fit.
llvm::Constant *CodeGen::stringLiteral(const std::u16string &text) {
  auto found = literals_.find(text);
  if (found != literals_.end())
    return found->second;

  bool twoByte = llvm::any_of(text, [](char16_t c) { return c > 0xFF; });
  bool ascii = llvm::all_of(text, [](char16_t c) { return c < 0x80; });
  llvm::Constant *value;
  if (text.empty()) {
    value = llvm::ConstantStruct::get(
        strTy_, {llvm::ConstantPointerNull::get(builder_.getPtrTy()), builder_.getInt64(kStrAscii)});
  } else if (!twoByte && text.size() <= kInlineMax) {
    // The first code unit goes to the lowest address, zeros after the last.
    bool little = module_->getDataLayout().isLittleEndian();
    uint64_t bits = 0;
    for (size_t i = 0; i < text.size(); ++i)
      bits |= uint64_t(static_cast<uint8_t>(text[i])) << (8 * (little ? i : 7 - i));
    uint64_t meta = text.size() | kStrRope | (ascii ? kStrAscii : 0);
    value = llvm::ConstantStruct::get(
        strTy_, {llvm::ConstantExpr::getIntToPtr(builder_.getInt64(bits), builder_.getPtrTy()),
                 builder_.getInt64(meta)});
  } else {
    llvm::Constant *data;
    if (twoByte) {
      std::vector<uint16_t> units(text.begin(), text.end());
      data = llvm::ConstantDataArray::get(ctx_, units);
    } else {
      std::vector<uint8_t> units;
      for (char16_t c : text)
        units.push_back(static_cast<uint8_t>(c));
      data = llvm::ConstantDataArray::get(ctx_, units);
    }
    llvm::Type *i32 = builder_.getInt32Ty();
    auto *layout = llvm::StructType::get(ctx_, {builder_.getInt64Ty(), i32, i32, i32, i32, data->getType()});
    auto length = static_cast<uint32_t>(text.size());
    llvm::Constant *init = llvm::ConstantStruct::get(
        layout, {builder_.getInt64(kLiteralRefCount),
                 builder_.getInt32(twoByte ? kKindBuffer2 : kKindBuffer1), builder_.getInt32(0),
                 builder_.getInt32(length), builder_.getInt32(length), data});
    auto *global = new llvm::GlobalVariable(*module_, layout, /*isConstant=*/true,
                                            llvm::GlobalValue::PrivateLinkage, init, ".str");
    global->setUnnamedAddr(llvm::GlobalValue::UnnamedAddr::Global);
    global->setAlignment(llvm::Align(8));
    uint64_t meta = length | (twoByte ? kStrTwoByte : 0) | (ascii ? kStrAscii : 0);
    value = llvm::ConstantStruct::get(strTy_, {global, builder_.getInt64(meta)});
  }
  literals_.emplace(text, value);
  return value;
}

} // namespace tz
