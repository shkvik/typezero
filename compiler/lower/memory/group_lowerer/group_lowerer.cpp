#include "compiler/lower/memory/group_lowerer/group_lowerer.h"
#include "compiler/lower/layouts/object_layouts/object_layouts.h"
#include "compiler/lower/layouts/type_descriptors/type_descriptors.h"
#include "compiler/lower/memory/store_lowerer/store_lowerer.h"
#include "compiler/lower/module_builder/module_builder.h"
#include "compiler/lower/places/place_lowerer/place_lowerer.h"
#include "compiler/lower/values/string_lowerer/string_lowerer.h"
#include "compiler/memory/memory_plan/memory_plan.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/IR/MDBuilder.h"
#include "llvm/Support/ConvertUTF.h"
#include <functional>
#include <map>
#include <set>

using llvm::BasicBlock;
using llvm::cast;
using llvm::dyn_cast;
using llvm::isa;
using llvm::Value;

namespace tz {

namespace {

enum class RefShape { None, Pointer, Function, Optional, Other };

}

GroupLowerer::GroupLowerer(Lowering &lowering) : lowering_(lowering) {}

llvm::FunctionCallee GroupLowerer::groupFunction(llvm::StringRef name, llvm::Type *ret,
                                                 llvm::ArrayRef<llvm::Type *> params) {
  llvm::FunctionCallee callee =
      lowering_.module_->getOrInsertFunction(name, llvm::FunctionType::get(ret, params, false));
  cast<llvm::Function>(callee.getCallee())->addFnAttr(llvm::Attribute::NoUnwind);

  return callee;
}

llvm::Value *GroupLowerer::allocMember(uint64_t size, const llvm::Twine &name) {
  llvm::FunctionCallee alloc = groupFunction("tz_group_alloc", lowering_.builder_.getPtrTy(),
                                             {lowering_.builder_.getInt64Ty()});
  cast<llvm::Function>(alloc.getCallee())->addRetAttr(llvm::Attribute::NoAlias);

  return lowering_.builder_.CreateCall(alloc, {lowering_.builder_.getInt64(size)}, name);
}

void GroupLowerer::adoptFields(llvm::Value *object, const Type *type) {
  if (!lowering_.isCyclic(type)) {
    return;
  }
  const Lowering::ObjectLayout &layout = lowering_.objectLayouts().objectLayout(type);
  llvm::ArrayRef<Field> fields = type->fields();
  for (unsigned i = 0; i < fields.size(); ++i) {
    if (lowering_.memory().mayHoldMember(fields[i].type)) {
      lowering_.storeLowerer().groupStore(
          object,
          lowering_.builder_.CreateLoad(
              lowering_.moduleBuilder().lower(fields[i].type),
              lowering_.builder_.CreateStructGEP(layout.type, object, layout.elements[i])),
          fields[i].type);
    }
  }
}

llvm::Constant *GroupLowerer::memberDescriptor(llvm::Function *drop, llvm::StringRef name) {
  llvm::Constant *&descriptor = lowering_.memberDescriptors_[drop];
  if (descriptor) {
    return descriptor;
  }
  llvm::Constant *null = llvm::ConstantPointerNull::get(lowering_.builder_.getPtrTy());
  llvm::Constant *init = llvm::ConstantStruct::get(
      lowering_.layoutDescTy_,
      {drop, null, lowering_.builder_.getInt32(0), lowering_.builder_.getInt32(0),
       lowering_.stringLowerer().utf8Constant(name),
       lowering_.builder_.getInt32(uint32_t(name.size())), lowering_.builder_.getInt32(0)});
  auto *global = new llvm::GlobalVariable(*lowering_.module_, lowering_.layoutDescTy_, true,
                                          llvm::GlobalValue::PrivateLinkage, init,
                                          drop->getName() + ".layout");
  global->setUnnamedAddr(llvm::GlobalValue::UnnamedAddr::Global);
  descriptor = global;

  return global;
}

llvm::Value *GroupLowerer::newArray(const Type *arrayType, llvm::Value *length,
                                    llvm::Value *capacity, const llvm::Twine &name) {
  return newArray(arrayType, length, capacity, lowering_.isCyclic(arrayType), name);
}

llvm::Value *GroupLowerer::newArray(const Type *arrayType, llvm::Value *length,
                                    llvm::Value *capacity, bool member, const llvm::Twine &name) {
  llvm::Constant *element = lowering_.typeDescriptors().typeDescriptor(arrayType->inner());
  if (!member) {
    return lowering_.builder_.CreateCall(lowering_.rt_.arrNew, {element, length, capacity}, name);
  }
  llvm::Type *ptr = lowering_.builder_.getPtrTy(), *i64 = lowering_.builder_.getInt64Ty();
  llvm::FunctionCallee alloc = groupFunction("tz_arr_new_group", ptr, {ptr, i64, i64});
  cast<llvm::Function>(alloc.getCallee())->addRetAttr(llvm::Attribute::NoAlias);

  return lowering_.builder_.CreateCall(alloc, {element, length, capacity}, name);
}

llvm::Function *GroupLowerer::groupAwareRelease(llvm::Function *plain) {
  llvm::Function *&fn = groupReleases_[plain];
  if (fn) {
    return fn;
  }
  llvm::Type *ptr = lowering_.builder_.getPtrTy();
  auto *type = llvm::FunctionType::get(lowering_.builder_.getVoidTy(), {ptr}, false);
  fn = llvm::Function::Create(type, llvm::GlobalValue::InternalLinkage, plain->getName() + ".g",
                              lowering_.module_.get());
  fn->addFnAttr(llvm::Attribute::AlwaysInline);
  fn->addFnAttr(llvm::Attribute::NoUnwind);
  llvm::IRBuilder<> b(llvm::BasicBlock::Create(lowering_.ctx_, "entry", fn));
  auto *countedBlock = llvm::BasicBlock::Create(lowering_.ctx_, "counted", fn);
  auto *memberBlock = llvm::BasicBlock::Create(lowering_.ctx_, "member", fn);
  auto *plainBlock = llvm::BasicBlock::Create(lowering_.ctx_, "plain", fn);
  auto *doneBlock = llvm::BasicBlock::Create(lowering_.ctx_, "done", fn);
  llvm::Value *object = fn->getArg(0);
  b.CreateCondBr(b.CreateIsNotNull(object), countedBlock, doneBlock);
  b.SetInsertPoint(countedBlock);
  llvm::Value *count = b.CreateLoad(b.getInt64Ty(), object, "rc");
  if (lowering_.parallel_) {
    auto *notMemberBlock = llvm::BasicBlock::Create(lowering_.ctx_, "not.member", fn);
    auto *sharedBlock = llvm::BasicBlock::Create(lowering_.ctx_, "shared", fn);
    auto *lastBlock = llvm::BasicBlock::Create(lowering_.ctx_, "shared.last", fn);
    b.CreateCondBr(b.CreateICmpSLT(count, b.getInt64(uint64_t(-(int64_t(1) << 60)))), memberBlock,
                   notMemberBlock);
    b.SetInsertPoint(notMemberBlock);
    b.CreateCondBr(b.CreateICmpSLT(count, b.getInt64(0)), sharedBlock, plainBlock);
    b.SetInsertPoint(sharedBlock);
    llvm::Value *old = b.CreateAtomicRMW(llvm::AtomicRMWInst::Sub, object, b.getInt64(1),
                                         llvm::MaybeAlign(8), llvm::AtomicOrdering::AcquireRelease);
    b.CreateCondBr(b.CreateICmpEQ(old, b.getInt64(uint64_t(-(int64_t(1) << 60) + 1))), lastBlock,
                   doneBlock);
    b.SetInsertPoint(lastBlock);
    b.CreateStore(b.getInt64(1), object);
    b.CreateBr(plainBlock);
  } else {
    b.CreateCondBr(b.CreateICmpSLT(count, b.getInt64(0)), memberBlock, plainBlock);
  }
  b.SetInsertPoint(memberBlock);
  b.CreateCall(groupFunction("tz_group_release", b.getVoidTy(), {ptr}), {object});
  b.CreateBr(doneBlock);
  b.SetInsertPoint(plainBlock);
  b.CreateCall(plain, {object});
  b.CreateBr(doneBlock);
  b.SetInsertPoint(doneBlock);
  b.CreateRetVoid();

  return fn;
}

void GroupLowerer::retainObject(llvm::Value *object) {
  lowering_.builder_.CreateCall(
      lowering_.memory().hasCycles ? lowering_.refRetainFn_ : lowering_.arrRetainFn_, {object});
}

llvm::Constant *GroupLowerer::groupSite() {
  llvm::Constant *frame = lowering_.placeLowerer().frameDescriptor(
      lowering_.storeLoc_.isValid() ? lowering_.storeLoc_ : lowering_.loc_);
  llvm::Constant *&site = groupSites_[{frame, lowering_.storeField_}];
  if (site) {
    return site;
  }
  llvm::Type *ptr = lowering_.builder_.getPtrTy(), *i32 = lowering_.builder_.getInt32Ty();
  auto *type = llvm::StructType::get(lowering_.ctx_, {ptr, ptr, i32});
  llvm::Constant *init = llvm::ConstantStruct::get(
      type, {frame, lowering_.stringLowerer().utf8Constant(lowering_.storeField_),
             lowering_.builder_.getInt32(uint32_t(lowering_.storeField_.size()))});
  auto *global = new llvm::GlobalVariable(*lowering_.module_, type, true,
                                          llvm::GlobalValue::PrivateLinkage, init, "tz.site");
  site = global;

  return site;
}

void GroupLowerer::noteUnstored(llvm::Value *object, const Expr *site) {
  if (!lowering_.debugMemory_ || !lowering_.memory().unstoredSites.contains(site)) {
    return;
  }
  llvm::Type *ptr = lowering_.builder_.getPtrTy();
  lowering_.builder_.CreateCall(
      groupFunction("tz_group_note_unstored", lowering_.builder_.getVoidTy(), {ptr, ptr}),
      {object, lowering_.placeLowerer().frameDescriptor(site->loc())});
}

}
