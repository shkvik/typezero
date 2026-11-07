#include "compiler/lower/memory/store_lowerer/store_lowerer.h"
#include "compiler/library/process/process_lowerer/process_lowerer.h"
#include "compiler/lower/function_frame/function_frame.h"
#include "compiler/lower/functions/closure_lowerer/closure_lowerer.h"
#include "compiler/lower/functions/env_lowerer/env_lowerer.h"
#include "compiler/lower/layouts/type_descriptors/type_descriptors.h"
#include "compiler/lower/memory/group_lowerer/group_lowerer.h"
#include "compiler/lower/memory/memory_debug/memory_debug.h"
#include "compiler/lower/memory/ref_counts/ref_counts.h"
#include "compiler/lower/module_builder/module_builder.h"
#include "compiler/lower/stmt/stmt_lowerer/stmt_lowerer.h"
#include "compiler/lower/types/optional_lowerer/optional_lowerer.h"
#include "compiler/memory/memory_plan/memory_plan.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/IR/MDBuilder.h"
#include <functional>

using llvm::cast;
using llvm::isa;

namespace tz {

namespace {

enum class RefShape { None, Pointer, Function, Optional, Other };

}

llvm::BasicBlock *StoreLowerer::blockHere(llvm::IRBuilder<> &b, const char *name) {
  return llvm::BasicBlock::Create(b.getContext(), name, b.GetInsertBlock()->getParent());
}

llvm::Value *StoreLowerer::spillHere(llvm::IRBuilder<> &b, llvm::Value *value, const char *name) {
  llvm::BasicBlock &entry = b.GetInsertBlock()->getParent()->getEntryBlock();
  llvm::IRBuilder<> at(&entry, entry.begin());
  llvm::AllocaInst *slot = at.CreateAlloca(value->getType(), nullptr, name);
  b.CreateStore(value, slot);

  return slot;
}

static RefShape refShape(const Type *type) {
  switch (type->kind()) {
  case Type::Kind::Object:
  case Type::Kind::Array:
  case Type::Kind::Dict:
  case Type::Kind::Unknown:
  case Type::Kind::Promise:
    return RefShape::Pointer;
  case Type::Kind::Function:
    return RefShape::Function;
  case Type::Kind::Union:
    return type->isTagged() ? RefShape::Other : RefShape::Pointer;
  case Type::Kind::Optional:
    return RefShape::Optional;
  case Type::Kind::Result:
    return RefShape::Other;
  default:
    return RefShape::None;
  }
}

StoreLowerer::StoreLowerer(Lowering &owner)
    : lowering(owner) {}

void StoreLowerer::groupStore(llvm::Value *owner, llvm::Value *value, const Type *type) {
  if (!this->lowering.memory().mayCarryMember(type) || isa<llvm::Constant>(value)) {
    return;
  }
  llvm::Type *ptr = this->lowering.builder_.getPtrTy();
  if (this->lowering.debugMemory_) {
    auto *site =
        cast<llvm::GlobalVariable>(this->lowering.module_->getOrInsertGlobal("tz_group_site", ptr));
    site->setThreadLocal(this->lowering.parallel_);
    this->lowering.builder_.CreateStore(
        this->lowering.groupLowerer().groupSite(),
        this->lowering.parallel_
            ? static_cast<llvm::Value *>(this->lowering.builder_.CreateThreadLocalAddress(site))
            : site);
  }
  switch (refShape(type)) {
  case RefShape::Pointer:
    this->lowering.builder_.CreateCall(
        this->lowering.groupLowerer().groupFunction(
            "tz_group_store", this->lowering.builder_.getVoidTy(), {ptr, ptr}),
        {owner, value});
    return;
  case RefShape::Function:
    this->lowering.builder_.CreateCall(
        this->lowering.groupLowerer().groupFunction(
            "tz_group_store", this->lowering.builder_.getVoidTy(), {ptr, ptr}),
        {owner, this->lowering.builder_.CreateExtractValue(value, 1)});
    return;
  case RefShape::Optional:
    this->groupStore(owner, this->lowering.optionalLowerer().optionalValue(value, type),
                     type->inner());
    return;
  case RefShape::Other:
    this->lowering.builder_.CreateCall(
        this->lowering.groupLowerer().groupFunction(
            "tz_group_store_value", this->lowering.builder_.getVoidTy(), {ptr, ptr, ptr}),
        {owner, this->lowering.typeDescriptors().typeDescriptor(type),
         StoreLowerer::spillHere(this->lowering.builder_, value, "stored")});
    return;
  case RefShape::None:
    return;
  }
}

void StoreLowerer::releaseIn(llvm::Value *owner, llvm::Value *value, const Type *type) {
  if (!this->lowering.memory().mayCarryMember(type) || isa<llvm::Constant>(value)) {
    this->lowering.refCounts().releaseValue(value, type);
    return;
  }
  llvm::Type *ptr = this->lowering.builder_.getPtrTy();
  auto viaGroup = [&](llvm::Value *ref, const std::function<void()> &plain) {
    llvm::BasicBlock *refBlock = StoreLowerer::blockHere(this->lowering.builder_, "in.ref");
    llvm::BasicBlock *memberBlock = StoreLowerer::blockHere(this->lowering.builder_, "in.member");
    llvm::BasicBlock *plainBlock = StoreLowerer::blockHere(this->lowering.builder_, "in.plain");
    llvm::BasicBlock *doneBlock = StoreLowerer::blockHere(this->lowering.builder_, "in.done");
    this->lowering.builder_.CreateCondBr(this->lowering.builder_.CreateIsNotNull(ref), refBlock,
                                         doneBlock);
    this->lowering.builder_.SetInsertPoint(refBlock);
    llvm::Value *count =
        this->lowering.builder_.CreateLoad(this->lowering.builder_.getInt64Ty(), ref, "rc");
    this->lowering.builder_.CreateCondBr(
        this->lowering.builder_.CreateICmpSLT(
            count, this->lowering.builder_.getInt64(uint64_t(-(int64_t(1) << 60)))),
        memberBlock, plainBlock);
    this->lowering.builder_.SetInsertPoint(memberBlock);
    this->lowering.builder_.CreateCall(
        this->lowering.groupLowerer().groupFunction(
            "tz_group_unstore", this->lowering.builder_.getVoidTy(), {ptr, ptr}),
        {owner, ref});
    this->lowering.builder_.CreateBr(doneBlock);
    this->lowering.builder_.SetInsertPoint(plainBlock);
    plain();
    this->lowering.builder_.CreateBr(doneBlock);
    this->lowering.builder_.SetInsertPoint(doneBlock);
  };
  switch (refShape(type)) {
  case RefShape::Pointer:
    viaGroup(value, [&] { this->lowering.refCounts().releaseValue(value, type); });
    return;
  case RefShape::Function:
    viaGroup(this->lowering.builder_.CreateExtractValue(value, 1),
             [&] { this->lowering.refCounts().releaseValue(value, type); });
    return;
  case RefShape::Optional:
    this->releaseIn(owner, this->lowering.optionalLowerer().optionalValue(value, type),
                    type->inner());
    return;
  case RefShape::Other:
    this->lowering.builder_.CreateCall(
        this->lowering.groupLowerer().groupFunction(
            "tz_value_release_in", this->lowering.builder_.getVoidTy(), {ptr, ptr, ptr}),
        {owner, this->lowering.typeDescriptors().typeDescriptor(type),
         StoreLowerer::spillHere(this->lowering.builder_, value, "released")});
    return;
  case RefShape::None:
    this->lowering.refCounts().releaseValue(value, type);
    return;
  }
}

void StoreLowerer::takeFrom(llvm::Value *owner, llvm::Value *value, const Type *type) {
  if (!this->lowering.memory().mayCarryMember(type) || isa<llvm::Constant>(value)) {
    return;
  }
  llvm::Type *ptr = this->lowering.builder_.getPtrTy();
  auto take = [&](llvm::Value *ref) {
    llvm::BasicBlock *refBlock = StoreLowerer::blockHere(this->lowering.builder_, "take.ref");
    llvm::BasicBlock *memberBlock = StoreLowerer::blockHere(this->lowering.builder_, "take.member");
    llvm::BasicBlock *doneBlock = StoreLowerer::blockHere(this->lowering.builder_, "take.done");
    this->lowering.builder_.CreateCondBr(this->lowering.builder_.CreateIsNotNull(ref), refBlock,
                                         doneBlock);
    this->lowering.builder_.SetInsertPoint(refBlock);
    llvm::Value *count =
        this->lowering.builder_.CreateLoad(this->lowering.builder_.getInt64Ty(), ref, "rc");
    this->lowering.builder_.CreateCondBr(
        this->lowering.builder_.CreateICmpSLT(
            count, this->lowering.builder_.getInt64(uint64_t(-(int64_t(1) << 60)))),
        memberBlock, doneBlock);
    this->lowering.builder_.SetInsertPoint(memberBlock);
    this->lowering.builder_.CreateCall(
        this->lowering.groupLowerer().groupFunction(
            "tz_group_take", this->lowering.builder_.getVoidTy(), {ptr, ptr}),
        {owner, ref});
    this->lowering.builder_.CreateBr(doneBlock);
    this->lowering.builder_.SetInsertPoint(doneBlock);
  };
  switch (refShape(type)) {
  case RefShape::Pointer:
    take(value);
    return;
  case RefShape::Function:
    take(this->lowering.builder_.CreateExtractValue(value, 1));
    return;
  case RefShape::Optional:
    this->takeFrom(owner, this->lowering.optionalLowerer().optionalValue(value, type),
                   type->inner());
    return;
  case RefShape::Other:
    this->lowering.builder_.CreateCall(
        this->lowering.groupLowerer().groupFunction(
            "tz_group_take_value", this->lowering.builder_.getVoidTy(), {ptr, ptr, ptr}),
        {owner, this->lowering.typeDescriptors().typeDescriptor(type),
         StoreLowerer::spillHere(this->lowering.builder_, value, "taken")});
    return;
  case RefShape::None:
    return;
  }
}

void StoreLowerer::storeBarrier(llvm::Value *owner, llvm::Value *value, const Type *type) {
  if (!this->lowering.parallel_ || !type->isManaged() || isa<llvm::Constant>(value)) {
    return;
  }
  llvm::Type *ptr = this->lowering.builder_.getPtrTy();
  llvm::Value *count =
      this->lowering.builder_.CreateLoad(this->lowering.builder_.getInt64Ty(), owner, "owner.rc");
  llvm::BasicBlock *barrierBlock = this->lowering.functionFrame().newBlock("barrier");
  llvm::BasicBlock *doneBlock = this->lowering.functionFrame().newBlock("barrier.done");
  this->lowering.builder_.CreateCondBr(
      this->lowering.builder_.CreateICmpSLT(count, this->lowering.builder_.getInt64(0)),
      barrierBlock, doneBlock, llvm::MDBuilder(this->lowering.ctx_).createUnlikelyBranchWeights());
  this->lowering.functionFrame().switchTo(barrierBlock);
  RefShape shape = type->isString() || type->isArray() ? RefShape::Other : refShape(type);
  if (shape == RefShape::Pointer || shape == RefShape::Function) {
    llvm::Value *object =
        shape == RefShape::Function ? this->lowering.builder_.CreateExtractValue(value, 1) : value;
    this->lowering.builder_.CreateCall(
        this->lowering.processLowerer().processFunction(
            "tz_store_barrier_ref", this->lowering.builder_.getVoidTy(), {ptr, ptr}),
        {owner, object});
  } else {
    this->lowering.builder_.CreateCall(
        this->lowering.processLowerer().processFunction(
            "tz_store_barrier", this->lowering.builder_.getVoidTy(), {ptr, ptr, ptr}),
        {owner, this->lowering.typeDescriptors().typeDescriptor(type),
         StoreLowerer::spillHere(this->lowering.builder_, value, "published")});
  }
  this->lowering.builder_.CreateBr(doneBlock);
  this->lowering.functionFrame().switchTo(doneBlock);
}

void StoreLowerer::publishValue(llvm::Value *value, const Type *type) {
  if (!this->lowering.parallel_ || !type->isManaged() || isa<llvm::Constant>(value)) {
    return;
  }
  llvm::Type *ptr = this->lowering.builder_.getPtrTy();
  this->lowering.builder_.CreateCall(
      this->lowering.processLowerer().processFunction(
          "tz_publish_value", this->lowering.builder_.getVoidTy(), {ptr, ptr}),
      {this->lowering.typeDescriptors().typeDescriptor(type),
       StoreLowerer::spillHere(this->lowering.builder_, value, "published")});
}

void StoreLowerer::storeIn(llvm::Value *owner, const Type *ownerType, llvm::Value *address,
                           llvm::Value *value, const Type *type) {
  this->storeBarrier(owner, value, type);
  if (!type->isManaged() || !this->lowering.memory().mayCarryMember(type) ||
      (ownerType && !this->lowering.ownerMayBeMember(ownerType))) {
    this->lowering.stmtLowerer().storeReplacing(address, value, type);
    return;
  }
  llvm::Value *previous =
      this->lowering.builder_.CreateLoad(this->lowering.moduleBuilder().lower(type), address);
  this->lowering.builder_.CreateStore(value, address);
  this->groupStore(owner, value, type);
  this->releaseIn(owner, previous, type);
}

void StoreLowerer::storeVar(const VarSymbol *var, llvm::Value *address, llvm::Value *value,
                            bool replacing) {
  const Type *type = ClosureLowerer::typeOf(var);
  if (var->isGlobal() && var->usedInFunction()) {
    this->publishValue(value, type);
  } else if (var->boxed() &&
             !(this->lowering.isCyclicBox(type) && this->lowering.memory().mayHoldMember(type))) {
    this->storeBarrier(this->lowering.envLowerer().boxPointer(var), value, type);
  }
  if (var->boxed() && this->lowering.isCyclicBox(type) &&
      this->lowering.memory().mayHoldMember(type)) {
    llvm::Value *box = this->lowering.envLowerer().boxPointer(var);
    if (replacing) {
      this->storeIn(box, nullptr, address, value, type);
    } else {
      this->lowering.builder_.CreateStore(value, address);
      this->groupStore(box, value, type);
    }
    return;
  }
  if (replacing) {
    this->lowering.stmtLowerer().storeReplacing(address, value, type);
  } else {
    this->lowering.builder_.CreateStore(value, address);
  }
}

void StoreLowerer::releaseObjectIn(llvm::Value *owner, llvm::Value *object) {
  llvm::Type *ptr = this->lowering.builder_.getPtrTy();
  llvm::BasicBlock *refBlock = StoreLowerer::blockHere(this->lowering.builder_, "in.ref");
  llvm::BasicBlock *memberBlock = StoreLowerer::blockHere(this->lowering.builder_, "in.member");
  llvm::BasicBlock *plainBlock = StoreLowerer::blockHere(this->lowering.builder_, "in.plain");
  llvm::BasicBlock *doneBlock = StoreLowerer::blockHere(this->lowering.builder_, "in.done");
  this->lowering.builder_.CreateCondBr(this->lowering.builder_.CreateIsNotNull(object), refBlock,
                                       doneBlock);
  this->lowering.builder_.SetInsertPoint(refBlock);
  llvm::Value *count =
      this->lowering.builder_.CreateLoad(this->lowering.builder_.getInt64Ty(), object, "rc");
  this->lowering.builder_.CreateCondBr(
      this->lowering.builder_.CreateICmpSLT(
          count, this->lowering.builder_.getInt64(uint64_t(-(int64_t(1) << 60)))),
      memberBlock, plainBlock);
  this->lowering.builder_.SetInsertPoint(memberBlock);
  this->lowering.builder_.CreateCall(
      this->lowering.groupLowerer().groupFunction("tz_group_unstore",
                                                  this->lowering.builder_.getVoidTy(), {ptr, ptr}),
      {owner, object});
  this->lowering.builder_.CreateBr(doneBlock);
  this->lowering.builder_.SetInsertPoint(plainBlock);
  this->lowering.builder_.CreateCall(this->lowering.plainObjReleaseFn_, {object});
  this->lowering.builder_.CreateBr(doneBlock);
  this->lowering.builder_.SetInsertPoint(doneBlock);
}

void StoreLowerer::forgetWeakRefs(llvm::Value *object) {
  llvm::Type *i64 = this->lowering.builder_.getInt64Ty();
  llvm::Value *count = this->lowering.builder_.CreateLoad(
      i64, this->lowering.module_->getOrInsertGlobal("tz_weak_count", i64));
  llvm::BasicBlock *forgetBlock = StoreLowerer::blockHere(this->lowering.builder_, "weak.forget");
  llvm::BasicBlock *doneBlock = StoreLowerer::blockHere(this->lowering.builder_, "weak.done");
  this->lowering.builder_.CreateCondBr(
      this->lowering.builder_.CreateICmpNE(count, this->lowering.builder_.getInt64(0)), forgetBlock,
      doneBlock);
  this->lowering.builder_.SetInsertPoint(forgetBlock);
  this->lowering.builder_.CreateCall(this->lowering.groupLowerer().groupFunction(
                                         "tz_weak_forget", this->lowering.builder_.getVoidTy(),
                                         {this->lowering.builder_.getPtrTy()}),
                                     {object});
  this->lowering.builder_.CreateBr(doneBlock);
  this->lowering.builder_.SetInsertPoint(doneBlock);
}

void StoreLowerer::emitMemberDrop(const Type *type, Lowering::ObjectLayout &layout) {
  llvm::Function *drop = layout.drop;
  llvm::Function *saved = this->lowering.function_;
  this->lowering.function_ = drop;
  this->lowering.builder_.SetInsertPoint(
      llvm::BasicBlock::Create(this->lowering.ctx_, "entry", drop));
  llvm::Value *object = drop->getArg(0);
  if (this->lowering.isWeakTarget(type)) {
    this->forgetWeakRefs(object);
  }
  llvm::ArrayRef<Field> fields = type->fields();
  for (unsigned i = 0; i < fields.size(); ++i) {
    const Type *fieldType = fields[i].type;
    if (!fieldType->isManaged()) {
      continue;
    }
    llvm::Value *address =
        this->lowering.builder_.CreateStructGEP(layout.type, object, layout.elements[i]);
    llvm::Value *value = this->lowering.builder_.CreateLoad(
        this->lowering.moduleBuilder().lower(fieldType), address, fields[i].name);
    this->lowering.memoryDebug().checkOwned(type, fields[i].name, value, fieldType);
    this->releaseIn(object, value, fieldType);
  }
  const ClassDecl *cls = type->classDecl();
  if (cls && cls->group) {
    const std::vector<VarSymbol *> &captures = cls->group->captures;
    for (size_t i = 0; i < captures.size(); ++i) {
      const VarSymbol *var = captures[i];
      llvm::Value *address = this->lowering.builder_.CreateStructGEP(layout.type, object,
                                                                     unsigned(layout.captures + i));
      if (var->boxed()) {
        this->releaseObjectIn(object,
                              this->lowering.builder_.CreateLoad(this->lowering.builder_.getPtrTy(),
                                                                 address, var->name + ".box"));
      } else if (ClosureLowerer::typeOf(var)->isManaged()) {
        const Type *varType = ClosureLowerer::typeOf(var);
        this->releaseIn(object,
                        this->lowering.builder_.CreateLoad(
                            this->lowering.moduleBuilder().lower(varType), address, var->name),
                        varType);
      }
    }
  }
  this->lowering.builder_.CreateCall(this->lowering.groupLowerer().groupFunction(
                                         "tz_group_obj_free", this->lowering.builder_.getVoidTy(),
                                         {this->lowering.builder_.getPtrTy()}),
                                     {object});
  this->lowering.builder_.CreateRetVoid();
  this->lowering.function_ = saved;
}

}
