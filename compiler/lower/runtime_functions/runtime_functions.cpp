#include "compiler/lower/runtime_functions/runtime_functions.h"
#include "compiler/lower/memory/group_lowerer/group_lowerer.h"
#include "compiler/memory/memory_plan/memory_plan.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/IR/Intrinsics.h"
#include "llvm/IR/MDBuilder.h"
#include "llvm/Support/ConvertUTF.h"
#include "llvm/Support/ErrorHandling.h"
#include <array>

using llvm::BasicBlock;
using llvm::cast;
using llvm::dyn_cast;
using llvm::isa;
using llvm::Value;

namespace tz {

namespace {

// The count of a shared object starts here (TZ_RC_SHARED,
// runtime/core/group/group.h); those of the members of groups are below it.
constexpr int64_t kRcShared = -(int64_t(1) << 60);

} // namespace

RuntimeFunctions::RuntimeFunctions(Lowering &lowering) : lowering_(lowering) {}

void RuntimeFunctions::declareRuntime() {
  llvm::Type *voidTy = lowering_.builder_.getVoidTy();
  llvm::Type *i32 = lowering_.builder_.getInt32Ty();
  llvm::Type *i64 = lowering_.builder_.getInt64Ty();
  llvm::Type *f64 = lowering_.builder_.getDoubleTy();
  llvm::Type *ptr = lowering_.builder_.getPtrTy();

  auto declare = [&](llvm::StringRef name, llvm::Type *ret, llvm::ArrayRef<llvm::Type *> params) {
    llvm::FunctionCallee callee =
        lowering_.module_->getOrInsertFunction(name, llvm::FunctionType::get(ret, params, false));
    cast<llvm::Function>(callee.getCallee())->addFnAttr(llvm::Attribute::NoUnwind);

    return callee;
  };
  lowering_.rt_.init = declare("tz_rt_init", voidTy, {});
  lowering_.rt_.shutdown = declare("tz_rt_shutdown", voidTy, {});
  lowering_.rt_.mainDone = declare("tz_rt_main_done", voidTy, {});
  lowering_.rt_.printI32 = declare("tz_print_i32", voidTy, {i32});
  lowering_.rt_.printI64 = declare("tz_print_i64", voidTy, {i64});
  lowering_.rt_.printF64 = declare("tz_print_f64", voidTy, {f64});
  lowering_.rt_.printBool = declare("tz_print_bool", voidTy, {i32});
  lowering_.rt_.printChar = declare("tz_print_char", voidTy, {i32});
  lowering_.rt_.printString = declare("tz_print_string", voidTy, {ptr, i64});
  lowering_.rt_.printValue = declare("tz_print_value", voidTy, {ptr, ptr});
  lowering_.rt_.debugger = declare("tz_debugger", voidTy, {});
  // Numbers (runtime/core/output/output.c, number.c and math.c).
  lowering_.rt_.toInt32 = declare("tz_to_int32", i32, {f64});
  lowering_.rt_.mathRound = declare("tz_math_round", f64, {f64});
  lowering_.rt_.mathPow = declare("tz_math_pow", f64, {f64, f64});
  lowering_.rt_.mathPowLong = declare("tz_math_pow_long", i64, {i64, i64});
  lowering_.rt_.mathHypot = declare("tz_math_hypot", f64, {ptr, i64});
  lowering_.rt_.mathRandom = declare("tz_math_random", f64, {});
  lowering_.rt_.checkedInt32OfLong = declare("tz_checked_int32_of_long", i32, {i64});
  lowering_.rt_.strToNumber = declare("tz_str_to_number", f64, {ptr, i64});
  lowering_.rt_.parseInt = declare("tz_parse_int", f64, {ptr, i64, i32});
  lowering_.rt_.parseFloat = declare("tz_parse_float", f64, {ptr, i64});
  lowering_.rt_.numToFixed = declare("tz_num_to_fixed", voidTy, {ptr, f64, i64});
  lowering_.rt_.numToRadix = declare("tz_num_to_radix", voidTy, {ptr, f64, i64});

  // String functions take strings as (obj, meta) pairs and return them
  // through a pointer passed first.
  lowering_.rt_.strFree = declare("tz_str_free", voidTy, {ptr});
  lowering_.rt_.strConcatN = declare("tz_str_concat_n", voidTy, {ptr, ptr, i64});
  lowering_.rt_.strEqual = declare("tz_str_equal", i32, {ptr, i64, ptr, i64});
  lowering_.rt_.strCompare = declare("tz_str_compare", i32, {ptr, i64, ptr, i64});
  lowering_.rt_.strCharAt = declare("tz_str_char_at", voidTy, {ptr, ptr, i64, i64});
  lowering_.rt_.strCharCodeAt = declare("tz_str_char_code_at", i32, {ptr, i64, i64});
  lowering_.rt_.strCodePointAt = declare("tz_str_code_point_at", i32, {ptr, ptr, i64, i64});
  lowering_.rt_.strSlice = declare("tz_str_slice", voidTy, {ptr, ptr, i64, i64, i64, i32});
  lowering_.rt_.strSubstring = declare("tz_str_substring", voidTy, {ptr, ptr, i64, i64, i64, i32});
  lowering_.rt_.strIndexOf = declare("tz_str_index_of", i32, {ptr, i64, ptr, i64, i64, i32});
  lowering_.rt_.strIncludes = declare("tz_str_includes", i32, {ptr, i64, ptr, i64, i64, i32});
  lowering_.rt_.strStartsWith = declare("tz_str_starts_with", i32, {ptr, i64, ptr, i64, i64, i32});
  lowering_.rt_.strEndsWith = declare("tz_str_ends_with", i32, {ptr, i64, ptr, i64, i64, i32});
  lowering_.rt_.strRepeat = declare("tz_str_repeat", voidTy, {ptr, ptr, i64, i64});
  lowering_.rt_.strFromI64 = declare("tz_str_from_i64", voidTy, {ptr, i64});
  lowering_.rt_.strFromF64 = declare("tz_str_from_f64", voidTy, {ptr, f64});
  lowering_.rt_.valueToString = declare("tz_value_to_string", voidTy, {ptr, ptr, ptr});
  lowering_.rt_.strCharAtOrEmpty = declare("tz_str_char_at_or_empty", voidTy, {ptr, ptr, i64, i64});
  lowering_.rt_.strCodePoint = declare("tz_str_code_point", i32, {ptr, i64, i64});
  lowering_.rt_.strLastIndexOf =
      declare("tz_str_last_index_of", i32, {ptr, i64, ptr, i64, i64, i32});
  lowering_.rt_.strSplit = declare("tz_str_split", ptr, {ptr, ptr, i64, ptr, i64, i32, i64, i32});
  lowering_.rt_.strTrim = declare("tz_str_trim", voidTy, {ptr, ptr, i64, i32});
  lowering_.rt_.strPad = declare("tz_str_pad", voidTy, {ptr, ptr, i64, i64, ptr, i64, i32, i32});
  lowering_.rt_.strToUpper = declare("tz_str_to_upper", voidTy, {ptr, ptr, i64});
  lowering_.rt_.strToLower = declare("tz_str_to_lower", voidTy, {ptr, ptr, i64});
  lowering_.rt_.strReplace =
      declare("tz_str_replace", voidTy, {ptr, ptr, i64, ptr, i64, ptr, i64, i32});
  lowering_.rt_.strFromCharCodes = declare("tz_str_from_char_codes", voidTy, {ptr, ptr, i64});
  lowering_.rt_.strFromCodePoints = declare("tz_str_from_code_points", voidTy, {ptr, ptr, i64});

  // Arrays (runtime/core/array/array.c). Values of any type go by address, with
  // the type descriptor of the elements stored in the array.
  lowering_.rt_.arrNew = declare("tz_arr_new", ptr, {ptr, i64, i64});
  lowering_.rt_.arrNewLength = declare("tz_arr_new_length", ptr, {ptr, i64});
  lowering_.rt_.arrFree = declare("tz_arr_free", voidTy, {ptr});
  lowering_.rt_.arrGrow = declare("tz_arr_grow", voidTy, {ptr, i64});
  lowering_.rt_.arrShift = declare("tz_arr_shift", i32, {ptr, ptr});
  lowering_.rt_.arrUnshiftSlots = declare("tz_arr_unshift_slots", ptr, {ptr, i64});
  lowering_.rt_.arrSetLength = declare("tz_arr_set_length", voidTy, {ptr, i64});
  lowering_.rt_.arrSlice = declare("tz_arr_slice", ptr, {ptr, i64, i64, i32});
  lowering_.rt_.arrCopy = declare("tz_arr_copy", ptr, {ptr});
  lowering_.rt_.arrAppend = declare("tz_arr_append", voidTy, {ptr, ptr});
  lowering_.rt_.arrAppendCodePoints = declare("tz_arr_append_code_points", voidTy, {ptr, ptr, i64});
  lowering_.rt_.arrIndexOf = declare("tz_arr_index_of", i32, {ptr, ptr, i64, i32, i32});
  lowering_.rt_.arrJoin = declare("tz_arr_join", voidTy, {ptr, ptr, ptr, i64, i32});
  lowering_.rt_.arrReverse = declare("tz_arr_reverse", voidTy, {ptr});
  lowering_.rt_.arrFill = declare("tz_arr_fill", voidTy, {ptr, ptr, i64, i64, i32, i32});
  lowering_.rt_.arrSort = declare("tz_arr_sort", voidTy, {ptr});
  lowering_.rt_.arrSortWith = declare("tz_arr_sort_with", voidTy, {ptr, ptr, ptr});
  lowering_.rt_.arrSplice = declare("tz_arr_splice", ptr, {ptr, i64, i64, i32, i64, ptr});
  // The objects of closures (runtime/core/closure/closure.c).
  lowering_.rt_.objAlloc = declare("tz_obj_alloc", ptr, {i64});
  lowering_.rt_.objFree = declare("tz_obj_free", voidTy, {ptr});
  // Object.keys and the like (runtime/core/object/object.c).
  lowering_.rt_.objKeys = declare("tz_obj_keys", ptr, {ptr, ptr});
  lowering_.rt_.objValues = declare("tz_obj_values", ptr, {ptr, ptr});
  lowering_.rt_.objEntries = declare("tz_obj_entries", ptr, {ptr, ptr, ptr, i64});

  // Functions that return memory nothing else points to yet.
  for (llvm::FunctionCallee fresh :
       {lowering_.rt_.arrNew, lowering_.rt_.arrNewLength, lowering_.rt_.arrSlice,
        lowering_.rt_.arrCopy, lowering_.rt_.objAlloc, lowering_.rt_.objKeys,
        lowering_.rt_.objValues, lowering_.rt_.objEntries}) {
    cast<llvm::Function>(fresh.getCallee())->addRetAttr(llvm::Attribute::NoAlias);
  }

  cast<llvm::Function>(lowering_.rt_.arrGrow.getCallee())->addFnAttr(llvm::Attribute::Cold);
}

// The inline fast paths of reference counting:
// - tz.str.retain / tz.str.release: only heap strings, whose meta has the sign
//   bit set, are counted;
// - tz.arr.retain / tz.arr.release: every array is counted; null (the zero
//   value of a slot) is skipped. tz.arr.retain serves objects too, whose
//   count is the first word as well (see codegen_object.cpp for releasing
//   them).
void RuntimeFunctions::defineRefCountHelpers() {
  for (bool isString : {true, false}) {
    llvm::Type *param =
        isString ? static_cast<llvm::Type *>(lowering_.strTy_) : lowering_.builder_.getPtrTy();
    auto *type = llvm::FunctionType::get(lowering_.builder_.getVoidTy(), {param}, false);
    for (bool isRetain : {true, false}) {
      std::string name =
          std::string(isString ? "tz.str." : "tz.arr.") + (isRetain ? "retain" : "release");
      auto *fn = llvm::Function::Create(type, llvm::GlobalValue::InternalLinkage, name,
                                        lowering_.module_.get());
      fn->addFnAttr(llvm::Attribute::AlwaysInline);
      fn->addFnAttr(llvm::Attribute::NoUnwind);

      llvm::IRBuilder<> b(llvm::BasicBlock::Create(lowering_.ctx_, "entry", fn));
      auto *countedBlock = llvm::BasicBlock::Create(lowering_.ctx_, "counted", fn);
      auto *doneBlock = llvm::BasicBlock::Create(lowering_.ctx_, "done", fn);
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
      auto *freeBlock = isRetain ? nullptr : llvm::BasicBlock::Create(lowering_.ctx_, "free", fn);
      // A shared object's count goes atomically
      // (runtime/core/publish/publish.c).
      if (lowering_.parallel_) {
        countShared(b, obj, count, isRetain, freeBlock, doneBlock);
      }
      if (isRetain) {
        b.CreateStore(b.CreateAdd(count, b.getInt64(1)), obj);
        b.CreateBr(doneBlock);
      } else {
        llvm::Value *left = b.CreateSub(count, b.getInt64(1));
        b.CreateStore(left, obj);
        b.CreateCondBr(b.CreateICmpEQ(left, b.getInt64(0)), freeBlock, doneBlock);
        b.SetInsertPoint(freeBlock);
        b.CreateCall(isString ? lowering_.rt_.strFree : lowering_.rt_.arrFree, {obj});
        b.CreateBr(doneBlock);
      }

      b.SetInsertPoint(doneBlock);
      b.CreateRetVoid();
      if (isString) {
        (isRetain ? lowering_.retainFn_ : lowering_.releaseFn_) = fn;
      } else {
        (isRetain ? lowering_.arrRetainFn_ : lowering_.arrReleaseFn_) = fn;
      }
    }
  }

  // tz.obj.retain / tz.obj.release: the objects of closures, which start with
  // { i64 rc, ptr descriptor }, where the descriptor starts with the drop
  // function; the last release calls it. Objects of the language start the
  // same way, so a function value's environment may be one (a bound method).
  auto *objHeader = llvm::StructType::get(
      lowering_.ctx_, {lowering_.builder_.getInt64Ty(), lowering_.builder_.getPtrTy()});
  auto *type = llvm::FunctionType::get(lowering_.builder_.getVoidTy(),
                                       {lowering_.builder_.getPtrTy()}, false);
  for (bool isRetain : {true, false}) {
    auto *fn = llvm::Function::Create(type, llvm::GlobalValue::InternalLinkage,
                                      isRetain ? "tz.obj.retain" : "tz.obj.release",
                                      lowering_.module_.get());
    fn->addFnAttr(llvm::Attribute::AlwaysInline);
    fn->addFnAttr(llvm::Attribute::NoUnwind);
    llvm::IRBuilder<> b(llvm::BasicBlock::Create(lowering_.ctx_, "entry", fn));
    auto *countedBlock = llvm::BasicBlock::Create(lowering_.ctx_, "counted", fn);
    auto *doneBlock = llvm::BasicBlock::Create(lowering_.ctx_, "done", fn);
    llvm::Value *obj = fn->getArg(0);
    b.CreateCondBr(b.CreateIsNotNull(obj), countedBlock, doneBlock);
    b.SetInsertPoint(countedBlock);
    llvm::Value *count = b.CreateLoad(b.getInt64Ty(), obj, "rc");
    auto *dropBlock = isRetain ? nullptr : llvm::BasicBlock::Create(lowering_.ctx_, "drop", fn);
    if (lowering_.parallel_) {
      countShared(b, obj, count, isRetain, dropBlock, doneBlock);
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
    (isRetain ? lowering_.objRetainFn_ : lowering_.objReleaseFn_) = fn;
  }
  lowering_.plainObjRetainFn_ = lowering_.objRetainFn_;
  lowering_.plainObjReleaseFn_ = lowering_.objReleaseFn_;

  // tz.ref.retain: a reference that may point at a member of a group, whose
  // count is negative (see runtime/core/group/group.c): its group's count goes
  // up.
  lowering_.refRetainFn_ = llvm::Function::Create(type, llvm::GlobalValue::InternalLinkage,
                                                  "tz.ref.retain", lowering_.module_.get());
  lowering_.refRetainFn_->addFnAttr(llvm::Attribute::AlwaysInline);
  lowering_.refRetainFn_->addFnAttr(llvm::Attribute::NoUnwind);
  {
    llvm::IRBuilder<> b(llvm::BasicBlock::Create(lowering_.ctx_, "entry", lowering_.refRetainFn_));
    auto *countedBlock =
        llvm::BasicBlock::Create(lowering_.ctx_, "counted", lowering_.refRetainFn_);
    auto *memberBlock = llvm::BasicBlock::Create(lowering_.ctx_, "member", lowering_.refRetainFn_);
    auto *plainBlock = llvm::BasicBlock::Create(lowering_.ctx_, "plain", lowering_.refRetainFn_);
    auto *doneBlock = llvm::BasicBlock::Create(lowering_.ctx_, "done", lowering_.refRetainFn_);
    llvm::Value *obj = lowering_.refRetainFn_->getArg(0);
    b.CreateCondBr(b.CreateIsNotNull(obj), countedBlock, doneBlock);
    b.SetInsertPoint(countedBlock);
    llvm::Value *count = b.CreateLoad(b.getInt64Ty(), obj, "rc");
    b.CreateCondBr(b.CreateICmpSLT(count, b.getInt64(0)), memberBlock, plainBlock);
    b.SetInsertPoint(memberBlock);
    b.CreateCall(
        lowering_.groupLowerer().groupFunction("tz_group_retain", b.getVoidTy(), {b.getPtrTy()}),
        {obj});
    b.CreateBr(doneBlock);
    b.SetInsertPoint(plainBlock);
    b.CreateStore(b.CreateAdd(count, b.getInt64(1)), obj);
    b.CreateBr(doneBlock);
    b.SetInsertPoint(doneBlock);
    b.CreateRetVoid();
  }
  // Environments, boxes and objects that code counts without knowing their
  // types may be members when the program has any.
  if (lowering_.memory().hasCycles) {
    lowering_.objRetainFn_ = lowering_.refRetainFn_;
    lowering_.objReleaseFn_ =
        lowering_.groupLowerer().groupAwareRelease(lowering_.plainObjReleaseFn_);
  }

  // tz.instance.release: an object of the language whose type the code does
  // not know (Type::anyObject); its header { i64 rc, ptr layout } leads to
  // the drop function, the first word of the layout.
  lowering_.instanceReleaseFn_ = llvm::Function::Create(
      type, llvm::GlobalValue::InternalLinkage, "tz.instance.release", lowering_.module_.get());
  lowering_.instanceReleaseFn_->addFnAttr(llvm::Attribute::AlwaysInline);
  lowering_.instanceReleaseFn_->addFnAttr(llvm::Attribute::NoUnwind);
  llvm::IRBuilder<> b(
      llvm::BasicBlock::Create(lowering_.ctx_, "entry", lowering_.instanceReleaseFn_));
  auto *countedBlock =
      llvm::BasicBlock::Create(lowering_.ctx_, "counted", lowering_.instanceReleaseFn_);
  auto *dropBlock = llvm::BasicBlock::Create(lowering_.ctx_, "drop", lowering_.instanceReleaseFn_);
  auto *doneBlock = llvm::BasicBlock::Create(lowering_.ctx_, "done", lowering_.instanceReleaseFn_);
  llvm::Value *obj = lowering_.instanceReleaseFn_->getArg(0);
  b.CreateCondBr(b.CreateIsNotNull(obj), countedBlock, doneBlock);
  b.SetInsertPoint(countedBlock);
  llvm::Value *instanceCount = b.CreateLoad(b.getInt64Ty(), obj, "rc");
  if (lowering_.parallel_) {
    countShared(b, obj, instanceCount, /*retain=*/false, dropBlock, doneBlock);
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
  lowering_.plainInstanceReleaseFn_ = lowering_.instanceReleaseFn_;
  if (lowering_.memory().hasCycles) {
    lowering_.instanceReleaseFn_ =
        lowering_.groupLowerer().groupAwareRelease(lowering_.plainInstanceReleaseFn_);
  }
}

// In a parallel program, the count of a shared object (below zero, see
// TZ_RC_SHARED in runtime/core/group/group.h) goes atomically; the last release
// drops the object, which is this thread's alone then, with a count of zero.
// Leaves `b` in the block for the other objects, whose `count` is the one
// loaded.
void RuntimeFunctions::countShared(llvm::IRBuilder<> &b, llvm::Value *obj, llvm::Value *count,
                                   bool retain, llvm::BasicBlock *freeBlock,
                                   llvm::BasicBlock *doneBlock) {
  llvm::Function *fn = b.GetInsertBlock()->getParent();
  auto *sharedBlock = llvm::BasicBlock::Create(lowering_.ctx_, "shared", fn);
  auto *plainBlock = llvm::BasicBlock::Create(lowering_.ctx_, "plain", fn);
  b.CreateCondBr(b.CreateICmpSLT(count, b.getInt64(0)), sharedBlock, plainBlock,
                 llvm::MDBuilder(lowering_.ctx_).createUnlikelyBranchWeights());
  b.SetInsertPoint(sharedBlock);
  if (retain) {
    b.CreateAtomicRMW(llvm::AtomicRMWInst::Add, obj, b.getInt64(1), llvm::MaybeAlign(8),
                      llvm::AtomicOrdering::Monotonic);
    b.CreateBr(doneBlock);
  } else {
    llvm::Value *old = b.CreateAtomicRMW(llvm::AtomicRMWInst::Sub, obj, b.getInt64(1),
                                         llvm::MaybeAlign(8), llvm::AtomicOrdering::AcquireRelease);
    auto *lastBlock = llvm::BasicBlock::Create(lowering_.ctx_, "shared.last", fn);
    b.CreateCondBr(b.CreateICmpEQ(old, b.getInt64(uint64_t(kRcShared + 1))), lastBlock, doneBlock);
    b.SetInsertPoint(lastBlock);
    b.CreateStore(b.getInt64(0), obj);
    b.CreateBr(freeBlock);
  }
  b.SetInsertPoint(plainBlock);
}

} // namespace tz
