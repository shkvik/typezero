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

using llvm::cast;

namespace tz {

namespace {

enum class RefShape { None, Pointer, Function, Optional, Other };

}

GroupLowerer::GroupLowerer(Lowering &owner)
    : lowering(owner) {}

llvm::FunctionCallee GroupLowerer::groupFunction(llvm::StringRef name, llvm::Type *ret,
                                                 llvm::ArrayRef<llvm::Type *> params) {
  llvm::FunctionCallee callee = this->lowering.module_->getOrInsertFunction(
      name, llvm::FunctionType::get(ret, params, false));
  cast<llvm::Function>(callee.getCallee())->addFnAttr(llvm::Attribute::NoUnwind);

  return callee;
}

llvm::Value *GroupLowerer::allocMember(uint64_t size, const llvm::Twine &name) {
  llvm::FunctionCallee alloc = this->groupFunction(
      "tz_group_alloc", this->lowering.builder_.getPtrTy(), {this->lowering.builder_.getInt64Ty()});
  cast<llvm::Function>(alloc.getCallee())->addRetAttr(llvm::Attribute::NoAlias);

  return this->lowering.builder_.CreateCall(alloc, {this->lowering.builder_.getInt64(size)}, name);
}

void GroupLowerer::adoptFields(llvm::Value *object, const Type *type) {
  if (!this->lowering.isCyclic(type)) {
    return;
  }
  const Lowering::ObjectLayout &layout = this->lowering.objectLayouts().objectLayout(type);
  llvm::ArrayRef<Field> fields = type->fields();
  for (unsigned i = 0; i < fields.size(); ++i) {
    if (this->lowering.memory().mayHoldMember(fields[i].type)) {
      this->lowering.storeLowerer().groupStore(
          object,
          this->lowering.builder_.CreateLoad(
              this->lowering.moduleBuilder().lower(fields[i].type),
              this->lowering.builder_.CreateStructGEP(layout.type, object, layout.elements[i])),
          fields[i].type);
    }
  }
}

llvm::Constant *GroupLowerer::memberDescriptor(llvm::Function *drop, llvm::StringRef name) {
  llvm::Constant *&descriptor = this->lowering.memberDescriptors_[drop];
  if (descriptor) {
    return descriptor;
  }
  llvm::Constant *null = llvm::ConstantPointerNull::get(this->lowering.builder_.getPtrTy());
  llvm::Constant *init = llvm::ConstantStruct::get(
      this->lowering.layoutDescTy_,
      {drop, null, this->lowering.builder_.getInt32(0), this->lowering.builder_.getInt32(0),
       this->lowering.stringLowerer().utf8Constant(name),
       this->lowering.builder_.getInt32(uint32_t(name.size())),
       this->lowering.builder_.getInt32(0)});
  auto *global = new llvm::GlobalVariable(*this->lowering.module_, this->lowering.layoutDescTy_,
                                          true, llvm::GlobalValue::PrivateLinkage, init,
                                          drop->getName() + ".layout");
  global->setUnnamedAddr(llvm::GlobalValue::UnnamedAddr::Global);
  descriptor = global;

  return global;
}

llvm::Value *GroupLowerer::newArray(const Type *arrayType, llvm::Value *length,
                                    llvm::Value *capacity, const llvm::Twine &name) {
  return this->newArray(arrayType, length, capacity, this->lowering.isCyclic(arrayType), name);
}

llvm::Value *GroupLowerer::newArray(const Type *arrayType, llvm::Value *length,
                                    llvm::Value *capacity, bool member, const llvm::Twine &name) {
  llvm::Constant *element = this->lowering.typeDescriptors().typeDescriptor(arrayType->inner());
  if (!member) {
    return this->lowering.builder_.CreateCall(this->lowering.rt_.arrNew,
                                              {element, length, capacity}, name);
  }
  llvm::Type *ptr = this->lowering.builder_.getPtrTy();
  llvm::Type *i64 = this->lowering.builder_.getInt64Ty();
  llvm::FunctionCallee alloc = this->groupFunction("tz_arr_new_group", ptr, {ptr, i64, i64});
  cast<llvm::Function>(alloc.getCallee())->addRetAttr(llvm::Attribute::NoAlias);

  return this->lowering.builder_.CreateCall(alloc, {element, length, capacity}, name);
}

llvm::Function *GroupLowerer::groupAwareRelease(llvm::Function *plain) {
  llvm::Function *&fn = this->groupReleases[plain];
  if (fn) {
    return fn;
  }
  llvm::Type *ptr = this->lowering.builder_.getPtrTy();
  auto *type = llvm::FunctionType::get(this->lowering.builder_.getVoidTy(), {ptr}, false);
  fn = llvm::Function::Create(type, llvm::GlobalValue::InternalLinkage, plain->getName() + ".g",
                              this->lowering.module_.get());
  fn->addFnAttr(llvm::Attribute::AlwaysInline);
  fn->addFnAttr(llvm::Attribute::NoUnwind);
  llvm::IRBuilder<> b(llvm::BasicBlock::Create(this->lowering.ctx_, "entry", fn));
  auto *countedBlock = llvm::BasicBlock::Create(this->lowering.ctx_, "counted", fn);
  auto *memberBlock = llvm::BasicBlock::Create(this->lowering.ctx_, "member", fn);
  auto *plainBlock = llvm::BasicBlock::Create(this->lowering.ctx_, "plain", fn);
  auto *doneBlock = llvm::BasicBlock::Create(this->lowering.ctx_, "done", fn);
  llvm::Value *object = fn->getArg(0);
  b.CreateCondBr(b.CreateIsNotNull(object), countedBlock, doneBlock);
  b.SetInsertPoint(countedBlock);
  llvm::Value *count = b.CreateLoad(b.getInt64Ty(), object, "rc");
  if (this->lowering.parallel_) {
    auto *notMemberBlock = llvm::BasicBlock::Create(this->lowering.ctx_, "not.member", fn);
    auto *sharedBlock = llvm::BasicBlock::Create(this->lowering.ctx_, "shared", fn);
    auto *lastBlock = llvm::BasicBlock::Create(this->lowering.ctx_, "shared.last", fn);
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
  b.CreateCall(this->groupFunction("tz_group_release", b.getVoidTy(), {ptr}), {object});
  b.CreateBr(doneBlock);
  b.SetInsertPoint(plainBlock);
  b.CreateCall(plain, {object});
  b.CreateBr(doneBlock);
  b.SetInsertPoint(doneBlock);
  b.CreateRetVoid();

  return fn;
}

void GroupLowerer::retainObject(llvm::Value *object) {
  this->lowering.builder_.CreateCall(
      this->lowering.memory().hasCycles ? this->lowering.refRetainFn_ : this->lowering.arrRetainFn_,
      {object});
}

llvm::Constant *GroupLowerer::groupSite() {
  llvm::Constant *frame = this->lowering.placeLowerer().frameDescriptor(
      this->lowering.storeLoc_.isValid() ? this->lowering.storeLoc_ : this->lowering.loc_);
  llvm::Constant *&site = this->groupSites[{frame, this->lowering.storeField_}];
  if (site) {
    return site;
  }
  llvm::Type *ptr = this->lowering.builder_.getPtrTy();
  llvm::Type *i32 = this->lowering.builder_.getInt32Ty();
  auto *type = llvm::StructType::get(this->lowering.ctx_, {ptr, ptr, i32});
  llvm::Constant *init = llvm::ConstantStruct::get(
      type, {frame, this->lowering.stringLowerer().utf8Constant(this->lowering.storeField_),
             this->lowering.builder_.getInt32(uint32_t(this->lowering.storeField_.size()))});
  auto *global = new llvm::GlobalVariable(*this->lowering.module_, type, true,
                                          llvm::GlobalValue::PrivateLinkage, init, "tz.site");
  site = global;

  return site;
}

void GroupLowerer::noteUnstored(llvm::Value *object, const Expr *site) {
  if (!this->lowering.debugMemory_ || !this->lowering.memory().unstoredSites.contains(site)) {
    return;
  }
  llvm::Type *ptr = this->lowering.builder_.getPtrTy();
  this->lowering.builder_.CreateCall(
      this->groupFunction("tz_group_note_unstored", this->lowering.builder_.getVoidTy(),
                          {ptr, ptr}),
      {object, this->lowering.placeLowerer().frameDescriptor(site->loc())});
}

}
