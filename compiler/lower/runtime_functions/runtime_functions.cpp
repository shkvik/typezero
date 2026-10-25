#include "compiler/lower/runtime_functions/runtime_functions.h"
#include "compiler/lower/memory/group_lowerer/group_lowerer.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/IR/Intrinsics.h"
#include "llvm/IR/MDBuilder.h"
#include "llvm/Support/ConvertUTF.h"
#include "llvm/Support/ErrorHandling.h"
#include <array>

using llvm::cast;
using llvm::dyn_cast;
using llvm::isa;
using llvm::BasicBlock;
using llvm::Value;

namespace tz {

namespace {

// The count of a shared object starts here (TZ_RC_SHARED,
// runtime/core/tzrt/tzrt.h); those of the members of groups are below it.
constexpr int64_t kRcShared = -(int64_t(1) << 60);

} // namespace

RuntimeFunctions::RuntimeFunctions(CodeGen &codegen) : codegen_(codegen) {}

void RuntimeFunctions::declareRuntime() {
  llvm::Type *voidTy = codegen_.builder_.getVoidTy();
  llvm::Type *i32 = codegen_.builder_.getInt32Ty();
  llvm::Type *i64 = codegen_.builder_.getInt64Ty();
  llvm::Type *f64 = codegen_.builder_.getDoubleTy();
  llvm::Type *ptr = codegen_.builder_.getPtrTy();

  auto declare = [&](llvm::StringRef name, llvm::Type *ret, llvm::ArrayRef<llvm::Type *> params) {
    llvm::FunctionCallee callee =
        codegen_.module_->getOrInsertFunction(name, llvm::FunctionType::get(ret, params, false));
    cast<llvm::Function>(callee.getCallee())->addFnAttr(llvm::Attribute::NoUnwind);

    return callee;
  };
  codegen_.rt_.init = declare("tz_rt_init", voidTy, {});
  codegen_.rt_.shutdown = declare("tz_rt_shutdown", voidTy, {});
  codegen_.rt_.mainDone = declare("tz_rt_main_done", voidTy, {});
  codegen_.rt_.printI32 = declare("tz_print_i32", voidTy, {i32});
  codegen_.rt_.printI64 = declare("tz_print_i64", voidTy, {i64});
  codegen_.rt_.printF64 = declare("tz_print_f64", voidTy, {f64});
  codegen_.rt_.printBool = declare("tz_print_bool", voidTy, {i32});
  codegen_.rt_.printChar = declare("tz_print_char", voidTy, {i32});
  codegen_.rt_.printString = declare("tz_print_string", voidTy, {ptr, i64});
  codegen_.rt_.printValue = declare("tz_print_value", voidTy, {ptr, ptr});
  codegen_.rt_.debugger = declare("tz_debugger", voidTy, {});
  // Numbers (runtime/core/tzrt/tzrt.c, number.c and math.c).
  codegen_.rt_.toInt32 = declare("tz_to_int32", i32, {f64});
  codegen_.rt_.mathRound = declare("tz_math_round", f64, {f64});
  codegen_.rt_.mathPow = declare("tz_math_pow", f64, {f64, f64});
  codegen_.rt_.mathPowLong = declare("tz_math_pow_long", i64, {i64, i64});
  codegen_.rt_.mathHypot = declare("tz_math_hypot", f64, {ptr, i64});
  codegen_.rt_.mathRandom = declare("tz_math_random", f64, {});
  codegen_.rt_.checkedInt32OfLong = declare("tz_checked_int32_of_long", i32, {i64});
  codegen_.rt_.strToNumber = declare("tz_str_to_number", f64, {ptr, i64});
  codegen_.rt_.parseInt = declare("tz_parse_int", f64, {ptr, i64, i32});
  codegen_.rt_.parseFloat = declare("tz_parse_float", f64, {ptr, i64});
  codegen_.rt_.numToFixed = declare("tz_num_to_fixed", voidTy, {ptr, f64, i64});
  codegen_.rt_.numToRadix = declare("tz_num_to_radix", voidTy, {ptr, f64, i64});

  // String functions take strings as (obj, meta) pairs and return them
  // through a pointer passed first.
  codegen_.rt_.strFree = declare("tz_str_free", voidTy, {ptr});
  codegen_.rt_.strConcatN = declare("tz_str_concat_n", voidTy, {ptr, ptr, i64});
  codegen_.rt_.strEqual = declare("tz_str_equal", i32, {ptr, i64, ptr, i64});
  codegen_.rt_.strCompare = declare("tz_str_compare", i32, {ptr, i64, ptr, i64});
  codegen_.rt_.strCharAt = declare("tz_str_char_at", voidTy, {ptr, ptr, i64, i64});
  codegen_.rt_.strCharCodeAt = declare("tz_str_char_code_at", i32, {ptr, i64, i64});
  codegen_.rt_.strCodePointAt = declare("tz_str_code_point_at", i32, {ptr, ptr, i64, i64});
  codegen_.rt_.strSlice = declare("tz_str_slice", voidTy, {ptr, ptr, i64, i64, i64, i32});
  codegen_.rt_.strSubstring = declare("tz_str_substring", voidTy, {ptr, ptr, i64, i64, i64, i32});
  codegen_.rt_.strIndexOf = declare("tz_str_index_of", i32, {ptr, i64, ptr, i64, i64, i32});
  codegen_.rt_.strIncludes = declare("tz_str_includes", i32, {ptr, i64, ptr, i64, i64, i32});
  codegen_.rt_.strStartsWith = declare("tz_str_starts_with", i32, {ptr, i64, ptr, i64, i64, i32});
  codegen_.rt_.strEndsWith = declare("tz_str_ends_with", i32, {ptr, i64, ptr, i64, i64, i32});
  codegen_.rt_.strRepeat = declare("tz_str_repeat", voidTy, {ptr, ptr, i64, i64});
  codegen_.rt_.strFromI64 = declare("tz_str_from_i64", voidTy, {ptr, i64});
  codegen_.rt_.strFromF64 = declare("tz_str_from_f64", voidTy, {ptr, f64});
  codegen_.rt_.valueToString = declare("tz_value_to_string", voidTy, {ptr, ptr, ptr});
  codegen_.rt_.strCharAtOrEmpty = declare("tz_str_char_at_or_empty", voidTy, {ptr, ptr, i64, i64});
  codegen_.rt_.strCodePoint = declare("tz_str_code_point", i32, {ptr, i64, i64});
  codegen_.rt_.strLastIndexOf = declare("tz_str_last_index_of", i32, {ptr, i64, ptr, i64, i64, i32});
  codegen_.rt_.strSplit = declare("tz_str_split", ptr, {ptr, ptr, i64, ptr, i64, i32, i64, i32});
  codegen_.rt_.strTrim = declare("tz_str_trim", voidTy, {ptr, ptr, i64, i32});
  codegen_.rt_.strPad = declare("tz_str_pad", voidTy, {ptr, ptr, i64, i64, ptr, i64, i32, i32});
  codegen_.rt_.strToUpper = declare("tz_str_to_upper", voidTy, {ptr, ptr, i64});
  codegen_.rt_.strToLower = declare("tz_str_to_lower", voidTy, {ptr, ptr, i64});
  codegen_.rt_.strReplace = declare("tz_str_replace", voidTy, {ptr, ptr, i64, ptr, i64, ptr, i64, i32});
  codegen_.rt_.strFromCharCodes = declare("tz_str_from_char_codes", voidTy, {ptr, ptr, i64});
  codegen_.rt_.strFromCodePoints = declare("tz_str_from_code_points", voidTy, {ptr, ptr, i64});

  // Arrays (runtime/core/array/array.c). Values of any type go by address, with
  // the type descriptor of the elements stored in the array.
  codegen_.rt_.arrNew = declare("tz_arr_new", ptr, {ptr, i64, i64});
  codegen_.rt_.arrNewLength = declare("tz_arr_new_length", ptr, {ptr, i64});
  codegen_.rt_.arrFree = declare("tz_arr_free", voidTy, {ptr});
  codegen_.rt_.arrGrow = declare("tz_arr_grow", voidTy, {ptr, i64});
  codegen_.rt_.arrShift = declare("tz_arr_shift", i32, {ptr, ptr});
  codegen_.rt_.arrUnshiftSlots = declare("tz_arr_unshift_slots", ptr, {ptr, i64});
  codegen_.rt_.arrSetLength = declare("tz_arr_set_length", voidTy, {ptr, i64});
  codegen_.rt_.arrSlice = declare("tz_arr_slice", ptr, {ptr, i64, i64, i32});
  codegen_.rt_.arrCopy = declare("tz_arr_copy", ptr, {ptr});
  codegen_.rt_.arrAppend = declare("tz_arr_append", voidTy, {ptr, ptr});
  codegen_.rt_.arrAppendCodePoints = declare("tz_arr_append_code_points", voidTy, {ptr, ptr, i64});
  codegen_.rt_.arrIndexOf = declare("tz_arr_index_of", i32, {ptr, ptr, i64, i32, i32});
  codegen_.rt_.arrJoin = declare("tz_arr_join", voidTy, {ptr, ptr, ptr, i64, i32});
  codegen_.rt_.arrReverse = declare("tz_arr_reverse", voidTy, {ptr});
  codegen_.rt_.arrFill = declare("tz_arr_fill", voidTy, {ptr, ptr, i64, i64, i32, i32});
  codegen_.rt_.arrSort = declare("tz_arr_sort", voidTy, {ptr});
  codegen_.rt_.arrSortWith = declare("tz_arr_sort_with", voidTy, {ptr, ptr, ptr});
  codegen_.rt_.arrSplice = declare("tz_arr_splice", ptr, {ptr, i64, i64, i32, i64, ptr});
  // The objects of closures (runtime/core/closure/closure.c).
  codegen_.rt_.objAlloc = declare("tz_obj_alloc", ptr, {i64});
  codegen_.rt_.objFree = declare("tz_obj_free", voidTy, {ptr});
  // Object.keys and the like (runtime/core/object/object.c).
  codegen_.rt_.objKeys = declare("tz_obj_keys", ptr, {ptr, ptr});
  codegen_.rt_.objValues = declare("tz_obj_values", ptr, {ptr, ptr});
  codegen_.rt_.objEntries = declare("tz_obj_entries", ptr, {ptr, ptr, ptr, i64});

  // Functions that return memory nothing else points to yet.
  for (llvm::FunctionCallee fresh : {codegen_.rt_.arrNew, codegen_.rt_.arrNewLength, codegen_.rt_.arrSlice, codegen_.rt_.arrCopy,
                                     codegen_.rt_.objAlloc, codegen_.rt_.objKeys, codegen_.rt_.objValues, codegen_.rt_.objEntries}) {
    cast<llvm::Function>(fresh.getCallee())->addRetAttr(llvm::Attribute::NoAlias);
  }

  cast<llvm::Function>(codegen_.rt_.arrGrow.getCallee())->addFnAttr(llvm::Attribute::Cold);
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
    llvm::Type *param = isString ? static_cast<llvm::Type *>(codegen_.strTy_) : codegen_.builder_.getPtrTy();
    auto *type = llvm::FunctionType::get(codegen_.builder_.getVoidTy(), {param}, false);
    for (bool isRetain : {true, false}) {
      std::string name =
          std::string(isString ? "tz.str." : "tz.arr.") + (isRetain ? "retain" : "release");
      auto *fn =
          llvm::Function::Create(type, llvm::GlobalValue::InternalLinkage, name, codegen_.module_.get());
      fn->addFnAttr(llvm::Attribute::AlwaysInline);
      fn->addFnAttr(llvm::Attribute::NoUnwind);

      llvm::IRBuilder<> b(llvm::BasicBlock::Create(codegen_.ctx_, "entry", fn));
      auto *countedBlock = llvm::BasicBlock::Create(codegen_.ctx_, "counted", fn);
      auto *doneBlock = llvm::BasicBlock::Create(codegen_.ctx_, "done", fn);
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
      auto *freeBlock = isRetain ? nullptr : llvm::BasicBlock::Create(codegen_.ctx_, "free", fn);
      // A shared object's count goes atomically
      // (runtime/core/publish/publish.c).
      if (codegen_.parallel_) {
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
        b.CreateCall(isString ? codegen_.rt_.strFree : codegen_.rt_.arrFree, {obj});
        b.CreateBr(doneBlock);
      }

      b.SetInsertPoint(doneBlock);
      b.CreateRetVoid();
      if (isString) {
        (isRetain ? codegen_.retainFn_ : codegen_.releaseFn_) = fn;
      } else {
        (isRetain ? codegen_.arrRetainFn_ : codegen_.arrReleaseFn_) = fn;
      }
    }
  }

  // tz.obj.retain / tz.obj.release: the objects of closures, which start with
  // { i64 rc, ptr descriptor }, where the descriptor starts with the drop
  // function; the last release calls it. Objects of the language start the
  // same way, so a function value's environment may be one (a bound method).
  auto *objHeader = llvm::StructType::get(codegen_.ctx_, {codegen_.builder_.getInt64Ty(), codegen_.builder_.getPtrTy()});
  auto *type = llvm::FunctionType::get(codegen_.builder_.getVoidTy(), {codegen_.builder_.getPtrTy()}, false);
  for (bool isRetain : {true, false}) {
    auto *fn = llvm::Function::Create(type, llvm::GlobalValue::InternalLinkage,
                                      isRetain ? "tz.obj.retain" : "tz.obj.release", codegen_.module_.get());
    fn->addFnAttr(llvm::Attribute::AlwaysInline);
    fn->addFnAttr(llvm::Attribute::NoUnwind);
    llvm::IRBuilder<> b(llvm::BasicBlock::Create(codegen_.ctx_, "entry", fn));
    auto *countedBlock = llvm::BasicBlock::Create(codegen_.ctx_, "counted", fn);
    auto *doneBlock = llvm::BasicBlock::Create(codegen_.ctx_, "done", fn);
    llvm::Value *obj = fn->getArg(0);
    b.CreateCondBr(b.CreateIsNotNull(obj), countedBlock, doneBlock);
    b.SetInsertPoint(countedBlock);
    llvm::Value *count = b.CreateLoad(b.getInt64Ty(), obj, "rc");
    auto *dropBlock = isRetain ? nullptr : llvm::BasicBlock::Create(codegen_.ctx_, "drop", fn);
    if (codegen_.parallel_) {
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
    (isRetain ? codegen_.objRetainFn_ : codegen_.objReleaseFn_) = fn;
  }
  codegen_.plainObjRetainFn_ = codegen_.objRetainFn_;
  codegen_.plainObjReleaseFn_ = codegen_.objReleaseFn_;

  // tz.ref.retain: a reference that may point at a member of a group, whose
  // count is negative (see runtime/core/group/group.c): its group's count goes
  // up.
  codegen_.refRetainFn_ = llvm::Function::Create(type, llvm::GlobalValue::InternalLinkage, "tz.ref.retain",
                                        codegen_.module_.get());
  codegen_.refRetainFn_->addFnAttr(llvm::Attribute::AlwaysInline);
  codegen_.refRetainFn_->addFnAttr(llvm::Attribute::NoUnwind);
  {
    llvm::IRBuilder<> b(llvm::BasicBlock::Create(codegen_.ctx_, "entry", codegen_.refRetainFn_));
    auto *countedBlock = llvm::BasicBlock::Create(codegen_.ctx_, "counted", codegen_.refRetainFn_);
    auto *memberBlock = llvm::BasicBlock::Create(codegen_.ctx_, "member", codegen_.refRetainFn_);
    auto *plainBlock = llvm::BasicBlock::Create(codegen_.ctx_, "plain", codegen_.refRetainFn_);
    auto *doneBlock = llvm::BasicBlock::Create(codegen_.ctx_, "done", codegen_.refRetainFn_);
    llvm::Value *obj = codegen_.refRetainFn_->getArg(0);
    b.CreateCondBr(b.CreateIsNotNull(obj), countedBlock, doneBlock);
    b.SetInsertPoint(countedBlock);
    llvm::Value *count = b.CreateLoad(b.getInt64Ty(), obj, "rc");
    b.CreateCondBr(b.CreateICmpSLT(count, b.getInt64(0)), memberBlock, plainBlock);
    b.SetInsertPoint(memberBlock);
    b.CreateCall(codegen_.groupLowerer().groupFunction("tz_group_retain", b.getVoidTy(), {b.getPtrTy()}), {obj});
    b.CreateBr(doneBlock);
    b.SetInsertPoint(plainBlock);
    b.CreateStore(b.CreateAdd(count, b.getInt64(1)), obj);
    b.CreateBr(doneBlock);
    b.SetInsertPoint(doneBlock);
    b.CreateRetVoid();
  }
  // Environments, boxes and objects that code counts without knowing their
  // types may be members when the program has any.
  if (codegen_.hasCycles_) {
    codegen_.objRetainFn_ = codegen_.refRetainFn_;
    codegen_.objReleaseFn_ = codegen_.groupLowerer().groupAwareRelease(codegen_.plainObjReleaseFn_);
  }

  // tz.instance.release: an object of the language whose type the code does
  // not know (Type::anyObject); its header { i64 rc, ptr layout } leads to
  // the drop function, the first word of the layout.
  codegen_.instanceReleaseFn_ = llvm::Function::Create(type, llvm::GlobalValue::InternalLinkage,
                                              "tz.instance.release", codegen_.module_.get());
  codegen_.instanceReleaseFn_->addFnAttr(llvm::Attribute::AlwaysInline);
  codegen_.instanceReleaseFn_->addFnAttr(llvm::Attribute::NoUnwind);
  llvm::IRBuilder<> b(llvm::BasicBlock::Create(codegen_.ctx_, "entry", codegen_.instanceReleaseFn_));
  auto *countedBlock = llvm::BasicBlock::Create(codegen_.ctx_, "counted", codegen_.instanceReleaseFn_);
  auto *dropBlock = llvm::BasicBlock::Create(codegen_.ctx_, "drop", codegen_.instanceReleaseFn_);
  auto *doneBlock = llvm::BasicBlock::Create(codegen_.ctx_, "done", codegen_.instanceReleaseFn_);
  llvm::Value *obj = codegen_.instanceReleaseFn_->getArg(0);
  b.CreateCondBr(b.CreateIsNotNull(obj), countedBlock, doneBlock);
  b.SetInsertPoint(countedBlock);
  llvm::Value *instanceCount = b.CreateLoad(b.getInt64Ty(), obj, "rc");
  if (codegen_.parallel_) {
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
  codegen_.plainInstanceReleaseFn_ = codegen_.instanceReleaseFn_;
  if (codegen_.hasCycles_) {
    codegen_.instanceReleaseFn_ = codegen_.groupLowerer().groupAwareRelease(codegen_.plainInstanceReleaseFn_);
  }
}

// In a parallel program, the count of a shared object (below zero, see
// TZ_RC_SHARED in runtime/core/tzrt/tzrt.h) goes atomically; the last release
// drops the object, which is this thread's alone then, with a count of zero.
// Leaves `b` in the block for the other objects, whose `count` is the one
// loaded.
void RuntimeFunctions::countShared(llvm::IRBuilder<> &b, llvm::Value *obj, llvm::Value *count, bool retain,
                          llvm::BasicBlock *freeBlock, llvm::BasicBlock *doneBlock) {
  llvm::Function *fn = b.GetInsertBlock()->getParent();
  auto *sharedBlock = llvm::BasicBlock::Create(codegen_.ctx_, "shared", fn);
  auto *plainBlock = llvm::BasicBlock::Create(codegen_.ctx_, "plain", fn);
  b.CreateCondBr(b.CreateICmpSLT(count, b.getInt64(0)), sharedBlock, plainBlock,
                 llvm::MDBuilder(codegen_.ctx_).createUnlikelyBranchWeights());
  b.SetInsertPoint(sharedBlock);
  if (retain) {
    b.CreateAtomicRMW(llvm::AtomicRMWInst::Add, obj, b.getInt64(1), llvm::MaybeAlign(8),
                      llvm::AtomicOrdering::Monotonic);
    b.CreateBr(doneBlock);
  } else {
    llvm::Value *old = b.CreateAtomicRMW(llvm::AtomicRMWInst::Sub, obj, b.getInt64(1),
                                         llvm::MaybeAlign(8), llvm::AtomicOrdering::AcquireRelease);
    auto *lastBlock = llvm::BasicBlock::Create(codegen_.ctx_, "shared.last", fn);
    b.CreateCondBr(b.CreateICmpEQ(old, b.getInt64(uint64_t(kRcShared + 1))), lastBlock, doneBlock);
    b.SetInsertPoint(lastBlock);
    b.CreateStore(b.getInt64(0), obj);
    b.CreateBr(freeBlock);
  }
  b.SetInsertPoint(plainBlock);
}

} // namespace tz
