#include "compiler/lower/runtime_functions/runtime_functions.h"
#include "compiler/lower/memory/group_lowerer/group_lowerer.h"
#include "compiler/memory/memory_plan/memory_plan.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/IR/Intrinsics.h"
#include "llvm/IR/MDBuilder.h"

using llvm::cast;

namespace tz {

namespace {

constexpr int64_t kRcShared = -(int64_t(1) << 60);

}

RuntimeFunctions::RuntimeFunctions(Lowering &owner)
    : lowering(owner) {}

void RuntimeFunctions::declareRuntime() {
  llvm::Type *voidTy = this->lowering.builder.getVoidTy();
  llvm::Type *i32 = this->lowering.builder.getInt32Ty();
  llvm::Type *i64 = this->lowering.builder.getInt64Ty();
  llvm::Type *f64 = this->lowering.builder.getDoubleTy();
  llvm::Type *ptr = this->lowering.builder.getPtrTy();

  auto declare = [&](llvm::StringRef name, llvm::Type *ret, llvm::ArrayRef<llvm::Type *> params) {
    llvm::FunctionCallee callee = this->lowering.module_->getOrInsertFunction(
        name, llvm::FunctionType::get(ret, params, false));
    cast<llvm::Function>(callee.getCallee())->addFnAttr(llvm::Attribute::NoUnwind);

    return callee;
  };
  this->lowering.rt_.init = declare("tz_rt_init", voidTy, {});
  this->lowering.rt_.shutdown = declare("tz_rt_shutdown", voidTy, {});
  this->lowering.rt_.mainDone = declare("tz_rt_main_done", voidTy, {});
  this->lowering.rt_.printU64 = declare("tz_print_u64", voidTy, {i64});
  this->lowering.rt_.printI64 = declare("tz_print_i64", voidTy, {i64});
  this->lowering.rt_.printF64 = declare("tz_print_f64", voidTy, {f64});
  this->lowering.rt_.printBool = declare("tz_print_bool", voidTy, {i32});
  this->lowering.rt_.printChar = declare("tz_print_char", voidTy, {i32});
  this->lowering.rt_.printString = declare("tz_print_string", voidTy, {ptr, i64});
  this->lowering.rt_.printValue = declare("tz_print_value", voidTy, {ptr, ptr});
  this->lowering.rt_.debugger = declare("tz_debugger", voidTy, {});
  this->lowering.rt_.toInt32 = declare("tz_to_int32", i32, {f64});
  this->lowering.rt_.mathRound = declare("tz_math_round", f64, {f64});
  this->lowering.rt_.mathPow = declare("tz_math_pow", f64, {f64, f64});
  this->lowering.rt_.mathPowLong = declare("tz_math_pow_long", i64, {i64, i64});
  this->lowering.rt_.mathHypot = declare("tz_math_hypot", f64, {ptr, i64});
  this->lowering.rt_.mathRandom = declare("tz_math_random", f64, {});
  this->lowering.rt_.strToNumber = declare("tz_str_to_number", f64, {ptr, i64});
  this->lowering.rt_.parseInt = declare("tz_parse_int", f64, {ptr, i64, i64});
  this->lowering.rt_.parseFloat = declare("tz_parse_float", f64, {ptr, i64});
  this->lowering.rt_.numToFixed = declare("tz_num_to_fixed", voidTy, {ptr, f64, i64});
  this->lowering.rt_.numToRadix = declare("tz_num_to_radix", voidTy, {ptr, f64, i64});

  this->lowering.rt_.strFree = declare("tz_str_free", voidTy, {ptr});
  this->lowering.rt_.strConcatN = declare("tz_str_concat_n", voidTy, {ptr, ptr, i64});
  this->lowering.rt_.strEqual = declare("tz_str_equal", i32, {ptr, i64, ptr, i64});
  this->lowering.rt_.strCompare = declare("tz_str_compare", i32, {ptr, i64, ptr, i64});
  this->lowering.rt_.strCharAt = declare("tz_str_char_at", voidTy, {ptr, ptr, i64, i64});
  this->lowering.rt_.strCharCodeAt = declare("tz_str_char_code_at", i32, {ptr, i64, i64});
  this->lowering.rt_.strCodePointAt = declare("tz_str_code_point_at", i32, {ptr, ptr, i64, i64});
  this->lowering.rt_.strSlice = declare("tz_str_slice", voidTy, {ptr, ptr, i64, i64, i64, i32});
  this->lowering.rt_.strSubstring =
      declare("tz_str_substring", voidTy, {ptr, ptr, i64, i64, i64, i32});
  this->lowering.rt_.strIndexOf = declare("tz_str_index_of", i32, {ptr, i64, ptr, i64, i64, i32});
  this->lowering.rt_.strIncludes = declare("tz_str_includes", i32, {ptr, i64, ptr, i64, i64, i32});
  this->lowering.rt_.strStartsWith =
      declare("tz_str_starts_with", i32, {ptr, i64, ptr, i64, i64, i32});
  this->lowering.rt_.strEndsWith = declare("tz_str_ends_with", i32, {ptr, i64, ptr, i64, i64, i32});
  this->lowering.rt_.strRepeat = declare("tz_str_repeat", voidTy, {ptr, ptr, i64, i64});
  this->lowering.rt_.strFromI64 = declare("tz_str_from_i64", voidTy, {ptr, i64});
  this->lowering.rt_.strFromU64 = declare("tz_str_from_u64", voidTy, {ptr, i64});
  this->lowering.rt_.strFromF64 = declare("tz_str_from_f64", voidTy, {ptr, f64});
  this->lowering.rt_.valueToString = declare("tz_value_to_string", voidTy, {ptr, ptr, ptr});
  this->lowering.rt_.strCharAtOrEmpty =
      declare("tz_str_char_at_or_empty", voidTy, {ptr, ptr, i64, i64});
  this->lowering.rt_.strCodePoint = declare("tz_str_code_point", i32, {ptr, i64, i64});
  this->lowering.rt_.strLastIndexOf =
      declare("tz_str_last_index_of", i32, {ptr, i64, ptr, i64, i64, i32});
  this->lowering.rt_.strSplit =
      declare("tz_str_split", ptr, {ptr, ptr, i64, ptr, i64, i32, i64, i32});
  this->lowering.rt_.strTrim = declare("tz_str_trim", voidTy, {ptr, ptr, i64, i32});
  this->lowering.rt_.strPad =
      declare("tz_str_pad", voidTy, {ptr, ptr, i64, i64, ptr, i64, i32, i32});
  this->lowering.rt_.strToUpper = declare("tz_str_to_upper", voidTy, {ptr, ptr, i64});
  this->lowering.rt_.strToLower = declare("tz_str_to_lower", voidTy, {ptr, ptr, i64});
  this->lowering.rt_.strReplace =
      declare("tz_str_replace", voidTy, {ptr, ptr, i64, ptr, i64, ptr, i64, i32});
  this->lowering.rt_.strFromCharCodes = declare("tz_str_from_char_codes", voidTy, {ptr, ptr, i64});
  this->lowering.rt_.strFromCodePoints =
      declare("tz_str_from_code_points", voidTy, {ptr, ptr, i64});

  this->lowering.rt_.arrNew = declare("tz_arr_new", ptr, {ptr, i64, i64});
  this->lowering.rt_.arrNewLength = declare("tz_arr_new_length", ptr, {ptr, i64});
  this->lowering.rt_.arrFree = declare("tz_arr_free", voidTy, {ptr});
  this->lowering.rt_.arrGrow = declare("tz_arr_grow", voidTy, {ptr, i64});
  this->lowering.rt_.arrShift = declare("tz_arr_shift", i32, {ptr, ptr});
  this->lowering.rt_.arrUnshiftSlots = declare("tz_arr_unshift_slots", ptr, {ptr, i64});
  this->lowering.rt_.arrSetLength = declare("tz_arr_set_length", voidTy, {ptr, i64});
  this->lowering.rt_.arrSlice = declare("tz_arr_slice", ptr, {ptr, i64, i64, i32});
  this->lowering.rt_.arrCopy = declare("tz_arr_copy", ptr, {ptr});
  this->lowering.rt_.arrAppend = declare("tz_arr_append", voidTy, {ptr, ptr});
  this->lowering.rt_.arrAppendCodePoints =
      declare("tz_arr_append_code_points", voidTy, {ptr, ptr, i64});
  this->lowering.rt_.arrIndexOf = declare("tz_arr_index_of", i32, {ptr, ptr, i64, i32, i32});
  this->lowering.rt_.arrJoin = declare("tz_arr_join", voidTy, {ptr, ptr, ptr, i64, i32});
  this->lowering.rt_.arrReverse = declare("tz_arr_reverse", voidTy, {ptr});
  this->lowering.rt_.arrFill = declare("tz_arr_fill", voidTy, {ptr, ptr, i64, i64, i32, i32});
  this->lowering.rt_.arrSort = declare("tz_arr_sort", voidTy, {ptr});
  this->lowering.rt_.arrSortWith = declare("tz_arr_sort_with", voidTy, {ptr, ptr, ptr});
  this->lowering.rt_.arrSplice = declare("tz_arr_splice", ptr, {ptr, i64, i64, i32, i64, ptr});
  this->lowering.rt_.objAlloc = declare("tz_obj_alloc", ptr, {i64});
  this->lowering.rt_.objFree = declare("tz_obj_free", voidTy, {ptr});
  this->lowering.rt_.objKeys = declare("tz_obj_keys", ptr, {ptr, ptr});
  this->lowering.rt_.objValues = declare("tz_obj_values", ptr, {ptr, ptr});
  this->lowering.rt_.objEntries = declare("tz_obj_entries", ptr, {ptr, ptr, ptr, i64});

  for (llvm::FunctionCallee fresh :
       {this->lowering.rt_.arrNew, this->lowering.rt_.arrNewLength, this->lowering.rt_.arrSlice,
        this->lowering.rt_.arrCopy, this->lowering.rt_.objAlloc, this->lowering.rt_.objKeys,
        this->lowering.rt_.objValues, this->lowering.rt_.objEntries}) {
    cast<llvm::Function>(fresh.getCallee())->addRetAttr(llvm::Attribute::NoAlias);
  }

  cast<llvm::Function>(this->lowering.rt_.arrGrow.getCallee())->addFnAttr(llvm::Attribute::Cold);
}

void RuntimeFunctions::defineRefCountHelpers() {
  for (bool isString : {true, false}) {
    llvm::Type *param = isString ? static_cast<llvm::Type *>(this->lowering.strTy)
                                 : this->lowering.builder.getPtrTy();
    auto *type = llvm::FunctionType::get(this->lowering.builder.getVoidTy(), {param}, false);
    for (bool isRetain : {true, false}) {
      std::string name =
          std::string(isString ? "tz.str." : "tz.arr.") + (isRetain ? "retain" : "release");
      auto *fn = llvm::Function::Create(type, llvm::GlobalValue::InternalLinkage, name,
                                        this->lowering.module_.get());
      fn->addFnAttr(llvm::Attribute::AlwaysInline);
      fn->addFnAttr(llvm::Attribute::NoUnwind);

      llvm::IRBuilder<> b(llvm::BasicBlock::Create(this->lowering.ctx, "entry", fn));
      auto *countedBlock = llvm::BasicBlock::Create(this->lowering.ctx, "counted", fn);
      auto *doneBlock = llvm::BasicBlock::Create(this->lowering.ctx, "done", fn);
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
      auto *freeBlock =
          isRetain ? nullptr : llvm::BasicBlock::Create(this->lowering.ctx, "free", fn);
      if (this->lowering.parallel_) {
        this->countShared(b, obj, count, isRetain, freeBlock, doneBlock);
      }
      if (isRetain) {
        b.CreateStore(b.CreateAdd(count, b.getInt64(1)), obj);
        b.CreateBr(doneBlock);
      } else {
        llvm::Value *left = b.CreateSub(count, b.getInt64(1));
        b.CreateStore(left, obj);
        b.CreateCondBr(b.CreateICmpEQ(left, b.getInt64(0)), freeBlock, doneBlock);
        b.SetInsertPoint(freeBlock);
        b.CreateCall(isString ? this->lowering.rt_.strFree : this->lowering.rt_.arrFree, {obj});
        b.CreateBr(doneBlock);
      }

      b.SetInsertPoint(doneBlock);
      b.CreateRetVoid();
      if (isString) {
        (isRetain ? this->lowering.retainFn : this->lowering.releaseFn) = fn;
      } else {
        (isRetain ? this->lowering.arrRetainFn : this->lowering.arrReleaseFn) = fn;
      }
    }
  }

  auto *objHeader = llvm::StructType::get(
      this->lowering.ctx, {this->lowering.builder.getInt64Ty(), this->lowering.builder.getPtrTy()});
  auto *type = llvm::FunctionType::get(this->lowering.builder.getVoidTy(),
                                       {this->lowering.builder.getPtrTy()}, false);
  for (bool isRetain : {true, false}) {
    auto *fn = llvm::Function::Create(type, llvm::GlobalValue::InternalLinkage,
                                      isRetain ? "tz.obj.retain" : "tz.obj.release",
                                      this->lowering.module_.get());
    fn->addFnAttr(llvm::Attribute::AlwaysInline);
    fn->addFnAttr(llvm::Attribute::NoUnwind);
    llvm::IRBuilder<> b(llvm::BasicBlock::Create(this->lowering.ctx, "entry", fn));
    auto *countedBlock = llvm::BasicBlock::Create(this->lowering.ctx, "counted", fn);
    auto *doneBlock = llvm::BasicBlock::Create(this->lowering.ctx, "done", fn);
    llvm::Value *obj = fn->getArg(0);
    b.CreateCondBr(b.CreateIsNotNull(obj), countedBlock, doneBlock);
    b.SetInsertPoint(countedBlock);
    llvm::Value *count = b.CreateLoad(b.getInt64Ty(), obj, "rc");
    auto *dropBlock = isRetain ? nullptr : llvm::BasicBlock::Create(this->lowering.ctx, "drop", fn);
    if (this->lowering.parallel_) {
      this->countShared(b, obj, count, isRetain, dropBlock, doneBlock);
    }
    if (isRetain) {
      b.CreateStore(b.CreateAdd(count, b.getInt64(1)), obj);
      b.CreateBr(doneBlock);
    } else {
      llvm::Value *left = b.CreateSub(count, b.getInt64(1));
      b.CreateStore(left, obj);
      b.CreateCondBr(b.CreateICmpEQ(left, b.getInt64(0)), dropBlock, doneBlock);
      b.SetInsertPoint(dropBlock);
      llvm::Value *descriptor =
          b.CreateLoad(b.getPtrTy(), b.CreateStructGEP(objHeader, obj, 1), "descriptor");
      llvm::Value *drop = b.CreateLoad(b.getPtrTy(), descriptor, "drop");
      b.CreateCall(type, drop, {obj});
      b.CreateBr(doneBlock);
    }
    b.SetInsertPoint(doneBlock);
    b.CreateRetVoid();
    (isRetain ? this->lowering.objRetainFn : this->lowering.objReleaseFn) = fn;
  }
  this->lowering.plainObjRetainFn = this->lowering.objRetainFn;
  this->lowering.plainObjReleaseFn = this->lowering.objReleaseFn;

  this->lowering.refRetainFn = llvm::Function::Create(
      type, llvm::GlobalValue::InternalLinkage, "tz.ref.retain", this->lowering.module_.get());
  this->lowering.refRetainFn->addFnAttr(llvm::Attribute::AlwaysInline);
  this->lowering.refRetainFn->addFnAttr(llvm::Attribute::NoUnwind);
  {
    llvm::IRBuilder<> b(
        llvm::BasicBlock::Create(this->lowering.ctx, "entry", this->lowering.refRetainFn));
    auto *countedBlock =
        llvm::BasicBlock::Create(this->lowering.ctx, "counted", this->lowering.refRetainFn);
    auto *memberBlock =
        llvm::BasicBlock::Create(this->lowering.ctx, "member", this->lowering.refRetainFn);
    auto *plainBlock =
        llvm::BasicBlock::Create(this->lowering.ctx, "plain", this->lowering.refRetainFn);
    auto *doneBlock =
        llvm::BasicBlock::Create(this->lowering.ctx, "done", this->lowering.refRetainFn);
    llvm::Value *obj = this->lowering.refRetainFn->getArg(0);
    b.CreateCondBr(b.CreateIsNotNull(obj), countedBlock, doneBlock);
    b.SetInsertPoint(countedBlock);
    llvm::Value *count = b.CreateLoad(b.getInt64Ty(), obj, "rc");
    b.CreateCondBr(b.CreateICmpSLT(count, b.getInt64(0)), memberBlock, plainBlock);
    b.SetInsertPoint(memberBlock);
    b.CreateCall(this->lowering.groupLowerer().groupFunction("tz_group_retain", b.getVoidTy(),
                                                             {b.getPtrTy()}),
                 {obj});
    b.CreateBr(doneBlock);
    b.SetInsertPoint(plainBlock);
    b.CreateStore(b.CreateAdd(count, b.getInt64(1)), obj);
    b.CreateBr(doneBlock);
    b.SetInsertPoint(doneBlock);
    b.CreateRetVoid();
  }
  if (this->lowering.memory().hasCycles) {
    this->lowering.objRetainFn = this->lowering.refRetainFn;
    this->lowering.objReleaseFn =
        this->lowering.groupLowerer().groupAwareRelease(this->lowering.plainObjReleaseFn);
  }

  this->lowering.instanceReleaseFn =
      llvm::Function::Create(type, llvm::GlobalValue::InternalLinkage, "tz.instance.release",
                             this->lowering.module_.get());
  this->lowering.instanceReleaseFn->addFnAttr(llvm::Attribute::AlwaysInline);
  this->lowering.instanceReleaseFn->addFnAttr(llvm::Attribute::NoUnwind);
  llvm::IRBuilder<> b(
      llvm::BasicBlock::Create(this->lowering.ctx, "entry", this->lowering.instanceReleaseFn));
  auto *countedBlock =
      llvm::BasicBlock::Create(this->lowering.ctx, "counted", this->lowering.instanceReleaseFn);
  auto *dropBlock =
      llvm::BasicBlock::Create(this->lowering.ctx, "drop", this->lowering.instanceReleaseFn);
  auto *doneBlock =
      llvm::BasicBlock::Create(this->lowering.ctx, "done", this->lowering.instanceReleaseFn);
  llvm::Value *obj = this->lowering.instanceReleaseFn->getArg(0);
  b.CreateCondBr(b.CreateIsNotNull(obj), countedBlock, doneBlock);
  b.SetInsertPoint(countedBlock);
  llvm::Value *instanceCount = b.CreateLoad(b.getInt64Ty(), obj, "rc");
  if (this->lowering.parallel_) {
    this->countShared(b, obj, instanceCount, false, dropBlock, doneBlock);
  }
  llvm::Value *left = b.CreateSub(instanceCount, b.getInt64(1));
  b.CreateStore(left, obj);
  b.CreateCondBr(b.CreateICmpEQ(left, b.getInt64(0)), dropBlock, doneBlock);
  b.SetInsertPoint(dropBlock);
  llvm::Value *layout = b.CreateLoad(b.getPtrTy(), b.CreateStructGEP(objHeader, obj, 1), "layout");
  llvm::Value *drop = b.CreateLoad(b.getPtrTy(), layout, "drop");
  b.CreateCall(type, drop, {obj});
  b.CreateBr(doneBlock);
  b.SetInsertPoint(doneBlock);
  b.CreateRetVoid();
  this->lowering.plainInstanceReleaseFn = this->lowering.instanceReleaseFn;
  if (this->lowering.memory().hasCycles) {
    this->lowering.instanceReleaseFn =
        this->lowering.groupLowerer().groupAwareRelease(this->lowering.plainInstanceReleaseFn);
  }
}

void RuntimeFunctions::countShared(llvm::IRBuilder<> &b, llvm::Value *obj, llvm::Value *count,
                                   bool retain, llvm::BasicBlock *freeBlock,
                                   llvm::BasicBlock *doneBlock) {
  llvm::Function *fn = b.GetInsertBlock()->getParent();
  auto *sharedBlock = llvm::BasicBlock::Create(this->lowering.ctx, "shared", fn);
  auto *plainBlock = llvm::BasicBlock::Create(this->lowering.ctx, "plain", fn);
  b.CreateCondBr(b.CreateICmpSLT(count, b.getInt64(0)), sharedBlock, plainBlock,
                 llvm::MDBuilder(this->lowering.ctx).createUnlikelyBranchWeights());
  b.SetInsertPoint(sharedBlock);
  if (retain) {
    b.CreateAtomicRMW(llvm::AtomicRMWInst::Add, obj, b.getInt64(1), llvm::MaybeAlign(8),
                      llvm::AtomicOrdering::Monotonic);
    b.CreateBr(doneBlock);
  } else {
    llvm::Value *old = b.CreateAtomicRMW(llvm::AtomicRMWInst::Sub, obj, b.getInt64(1),
                                         llvm::MaybeAlign(8), llvm::AtomicOrdering::AcquireRelease);
    auto *lastBlock = llvm::BasicBlock::Create(this->lowering.ctx, "shared.last", fn);
    b.CreateCondBr(b.CreateICmpEQ(old, b.getInt64(uint64_t(kRcShared + 1))), lastBlock, doneBlock);
    b.SetInsertPoint(lastBlock);
    b.CreateStore(b.getInt64(0), obj);
    b.CreateBr(freeBlock);
  }
  b.SetInsertPoint(plainBlock);
}

}
