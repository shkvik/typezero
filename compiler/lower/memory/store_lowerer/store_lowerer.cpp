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

llvm::BasicBlock *blockHere(llvm::IRBuilder<> &b, const char *name) {
  return llvm::BasicBlock::Create(b.getContext(), name, b.GetInsertBlock()->getParent());
}

llvm::Value *spillHere(llvm::IRBuilder<> &b, llvm::Value *value, const char *name) {
  llvm::BasicBlock &entry = b.GetInsertBlock()->getParent()->getEntryBlock();
  llvm::IRBuilder<> at(&entry, entry.begin());
  llvm::AllocaInst *slot = at.CreateAlloca(value->getType(), nullptr, name);
  b.CreateStore(value, slot);

  return slot;
}

enum class RefShape { None, Pointer, Function, Optional, Other };

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

StoreLowerer::StoreLowerer(Lowering &lowering) : lowering_(lowering) {}

void StoreLowerer::groupStore(llvm::Value *owner, llvm::Value *value, const Type *type) {
  if (!lowering_.memory().mayHoldMember(type) || isa<llvm::Constant>(value)) {
    return;
  }
  llvm::Type *ptr = lowering_.builder_.getPtrTy();
  if (lowering_.debugMemory_) {
    auto *site =
        cast<llvm::GlobalVariable>(lowering_.module_->getOrInsertGlobal("tz_group_site", ptr));
    site->setThreadLocal(lowering_.parallel_);
    lowering_.builder_.CreateStore(
        lowering_.groupLowerer().groupSite(),
        lowering_.parallel_
            ? static_cast<llvm::Value *>(lowering_.builder_.CreateThreadLocalAddress(site))
            : site);
  }
  switch (refShape(type)) {
  case RefShape::Pointer:
    lowering_.builder_.CreateCall(lowering_.groupLowerer().groupFunction(
                                      "tz_group_store", lowering_.builder_.getVoidTy(), {ptr, ptr}),
                                  {owner, value});
    return;
  case RefShape::Function:
    lowering_.builder_.CreateCall(lowering_.groupLowerer().groupFunction(
                                      "tz_group_store", lowering_.builder_.getVoidTy(), {ptr, ptr}),
                                  {owner, lowering_.builder_.CreateExtractValue(value, 1)});
    return;
  case RefShape::Optional:
    groupStore(owner, lowering_.optionalLowerer().optionalValue(value, type), type->inner());
    return;
  case RefShape::Other:
    lowering_.builder_.CreateCall(
        lowering_.groupLowerer().groupFunction("tz_group_store_value",
                                               lowering_.builder_.getVoidTy(), {ptr, ptr, ptr}),
        {owner, lowering_.typeDescriptors().typeDescriptor(type),
         spillHere(lowering_.builder_, value, "stored")});
    return;
  case RefShape::None:
    return;
  }
}

void StoreLowerer::releaseIn(llvm::Value *owner, llvm::Value *value, const Type *type) {
  if (!lowering_.memory().mayHoldMember(type) || isa<llvm::Constant>(value)) {
    lowering_.refCounts().releaseValue(value, type);
    return;
  }
  llvm::Type *ptr = lowering_.builder_.getPtrTy();
  auto viaGroup = [&](llvm::Value *ref, const std::function<void()> &plain) {
    llvm::BasicBlock *refBlock = blockHere(lowering_.builder_, "in.ref");
    llvm::BasicBlock *memberBlock = blockHere(lowering_.builder_, "in.member");
    llvm::BasicBlock *plainBlock = blockHere(lowering_.builder_, "in.plain");
    llvm::BasicBlock *doneBlock = blockHere(lowering_.builder_, "in.done");
    lowering_.builder_.CreateCondBr(lowering_.builder_.CreateIsNotNull(ref), refBlock, doneBlock);
    lowering_.builder_.SetInsertPoint(refBlock);
    llvm::Value *count = lowering_.builder_.CreateLoad(lowering_.builder_.getInt64Ty(), ref, "rc");
    lowering_.builder_.CreateCondBr(
        lowering_.builder_.CreateICmpSLT(
            count, lowering_.builder_.getInt64(uint64_t(-(int64_t(1) << 60)))),
        memberBlock, plainBlock);
    lowering_.builder_.SetInsertPoint(memberBlock);
    lowering_.builder_.CreateCall(
        lowering_.groupLowerer().groupFunction("tz_group_unstore", lowering_.builder_.getVoidTy(),
                                               {ptr, ptr}),
        {owner, ref});
    lowering_.builder_.CreateBr(doneBlock);
    lowering_.builder_.SetInsertPoint(plainBlock);
    plain();
    lowering_.builder_.CreateBr(doneBlock);
    lowering_.builder_.SetInsertPoint(doneBlock);
  };
  switch (refShape(type)) {
  case RefShape::Pointer:
    viaGroup(value, [&] { lowering_.refCounts().releaseValue(value, type); });
    return;
  case RefShape::Function:
    viaGroup(lowering_.builder_.CreateExtractValue(value, 1),
             [&] { lowering_.refCounts().releaseValue(value, type); });
    return;
  case RefShape::Optional:
    releaseIn(owner, lowering_.optionalLowerer().optionalValue(value, type), type->inner());
    return;
  case RefShape::Other:
    lowering_.builder_.CreateCall(
        lowering_.groupLowerer().groupFunction("tz_value_release_in",
                                               lowering_.builder_.getVoidTy(), {ptr, ptr, ptr}),
        {owner, lowering_.typeDescriptors().typeDescriptor(type),
         spillHere(lowering_.builder_, value, "released")});
    return;
  case RefShape::None:
    lowering_.refCounts().releaseValue(value, type);
    return;
  }
}

void StoreLowerer::takeFrom(llvm::Value *owner, llvm::Value *value, const Type *type) {
  if (!lowering_.memory().mayHoldMember(type) || isa<llvm::Constant>(value)) {
    return;
  }
  llvm::Type *ptr = lowering_.builder_.getPtrTy();
  auto take = [&](llvm::Value *ref) {
    llvm::BasicBlock *refBlock = blockHere(lowering_.builder_, "take.ref");
    llvm::BasicBlock *memberBlock = blockHere(lowering_.builder_, "take.member");
    llvm::BasicBlock *doneBlock = blockHere(lowering_.builder_, "take.done");
    lowering_.builder_.CreateCondBr(lowering_.builder_.CreateIsNotNull(ref), refBlock, doneBlock);
    lowering_.builder_.SetInsertPoint(refBlock);
    llvm::Value *count = lowering_.builder_.CreateLoad(lowering_.builder_.getInt64Ty(), ref, "rc");
    lowering_.builder_.CreateCondBr(
        lowering_.builder_.CreateICmpSLT(
            count, lowering_.builder_.getInt64(uint64_t(-(int64_t(1) << 60)))),
        memberBlock, doneBlock);
    lowering_.builder_.SetInsertPoint(memberBlock);
    lowering_.builder_.CreateCall(lowering_.groupLowerer().groupFunction(
                                      "tz_group_take", lowering_.builder_.getVoidTy(), {ptr, ptr}),
                                  {owner, ref});
    lowering_.builder_.CreateBr(doneBlock);
    lowering_.builder_.SetInsertPoint(doneBlock);
  };
  switch (refShape(type)) {
  case RefShape::Pointer:
    take(value);
    return;
  case RefShape::Function:
    take(lowering_.builder_.CreateExtractValue(value, 1));
    return;
  case RefShape::Optional:
    takeFrom(owner, lowering_.optionalLowerer().optionalValue(value, type), type->inner());
    return;
  case RefShape::Other:
    lowering_.builder_.CreateCall(
        lowering_.groupLowerer().groupFunction("tz_group_take_value",
                                               lowering_.builder_.getVoidTy(), {ptr, ptr, ptr}),
        {owner, lowering_.typeDescriptors().typeDescriptor(type),
         spillHere(lowering_.builder_, value, "taken")});
    return;
  case RefShape::None:
    return;
  }
}

void StoreLowerer::storeBarrier(llvm::Value *owner, llvm::Value *value, const Type *type) {
  if (!lowering_.parallel_ || !type->isManaged() || isa<llvm::Constant>(value)) {
    return;
  }
  llvm::Type *ptr = lowering_.builder_.getPtrTy();
  llvm::Value *count =
      lowering_.builder_.CreateLoad(lowering_.builder_.getInt64Ty(), owner, "owner.rc");
  llvm::BasicBlock *barrierBlock = lowering_.functionFrame().newBlock("barrier");
  llvm::BasicBlock *doneBlock = lowering_.functionFrame().newBlock("barrier.done");
  lowering_.builder_.CreateCondBr(
      lowering_.builder_.CreateICmpSLT(count, lowering_.builder_.getInt64(0)), barrierBlock,
      doneBlock, llvm::MDBuilder(lowering_.ctx_).createUnlikelyBranchWeights());
  lowering_.functionFrame().switchTo(barrierBlock);
  RefShape shape = type->isString() || type->isArray() ? RefShape::Other : refShape(type);
  if (shape == RefShape::Pointer || shape == RefShape::Function) {
    llvm::Value *object =
        shape == RefShape::Function ? lowering_.builder_.CreateExtractValue(value, 1) : value;
    lowering_.builder_.CreateCall(
        lowering_.processLowerer().processFunction("tz_store_barrier_ref",
                                                   lowering_.builder_.getVoidTy(), {ptr, ptr}),
        {owner, object});
  } else {
    lowering_.builder_.CreateCall(
        lowering_.processLowerer().processFunction("tz_store_barrier",
                                                   lowering_.builder_.getVoidTy(), {ptr, ptr, ptr}),
        {owner, lowering_.typeDescriptors().typeDescriptor(type),
         spillHere(lowering_.builder_, value, "published")});
  }
  lowering_.builder_.CreateBr(doneBlock);
  lowering_.functionFrame().switchTo(doneBlock);
}

void StoreLowerer::publishValue(llvm::Value *value, const Type *type) {
  if (!lowering_.parallel_ || !type->isManaged() || isa<llvm::Constant>(value)) {
    return;
  }
  llvm::Type *ptr = lowering_.builder_.getPtrTy();
  lowering_.builder_.CreateCall(lowering_.processLowerer().processFunction(
                                    "tz_publish_value", lowering_.builder_.getVoidTy(), {ptr, ptr}),
                                {lowering_.typeDescriptors().typeDescriptor(type),
                                 spillHere(lowering_.builder_, value, "published")});
}

void StoreLowerer::storeIn(llvm::Value *owner, const Type *ownerType, llvm::Value *address,
                           llvm::Value *value, const Type *type) {
  storeBarrier(owner, value, type);
  if (!type->isManaged() || !lowering_.memory().mayHoldMember(type) ||
      (ownerType && !lowering_.ownerMayBeMember(ownerType))) {
    lowering_.stmtLowerer().storeReplacing(address, value, type);
    return;
  }
  llvm::Value *previous =
      lowering_.builder_.CreateLoad(lowering_.moduleBuilder().lower(type), address);
  lowering_.builder_.CreateStore(value, address);
  groupStore(owner, value, type);
  releaseIn(owner, previous, type);
}

void StoreLowerer::storeVar(const VarSymbol *var, llvm::Value *address, llvm::Value *value,
                            bool replacing) {
  const Type *type = ClosureLowerer::typeOf(var);
  if (var->isGlobal && var->usedInFunction) {
    publishValue(value, type);
  } else if (var->boxed() &&
             !(lowering_.isCyclicBox(type) && lowering_.memory().mayHoldMember(type))) {
    storeBarrier(lowering_.envLowerer().boxPointer(var), value, type);
  }
  if (var->boxed() && lowering_.isCyclicBox(type) && lowering_.memory().mayHoldMember(type)) {
    llvm::Value *box = lowering_.envLowerer().boxPointer(var);
    if (replacing) {
      storeIn(box, nullptr, address, value, type);
    } else {
      lowering_.builder_.CreateStore(value, address);
      groupStore(box, value, type);
    }
    return;
  }
  if (replacing) {
    lowering_.stmtLowerer().storeReplacing(address, value, type);
  } else {
    lowering_.builder_.CreateStore(value, address);
  }
}

void StoreLowerer::releaseObjectIn(llvm::Value *owner, llvm::Value *object) {
  llvm::Type *ptr = lowering_.builder_.getPtrTy();
  llvm::BasicBlock *refBlock = blockHere(lowering_.builder_, "in.ref");
  llvm::BasicBlock *memberBlock = blockHere(lowering_.builder_, "in.member");
  llvm::BasicBlock *plainBlock = blockHere(lowering_.builder_, "in.plain");
  llvm::BasicBlock *doneBlock = blockHere(lowering_.builder_, "in.done");
  lowering_.builder_.CreateCondBr(lowering_.builder_.CreateIsNotNull(object), refBlock, doneBlock);
  lowering_.builder_.SetInsertPoint(refBlock);
  llvm::Value *count = lowering_.builder_.CreateLoad(lowering_.builder_.getInt64Ty(), object, "rc");
  lowering_.builder_.CreateCondBr(
      lowering_.builder_.CreateICmpSLT(count,
                                       lowering_.builder_.getInt64(uint64_t(-(int64_t(1) << 60)))),
      memberBlock, plainBlock);
  lowering_.builder_.SetInsertPoint(memberBlock);
  lowering_.builder_.CreateCall(lowering_.groupLowerer().groupFunction(
                                    "tz_group_unstore", lowering_.builder_.getVoidTy(), {ptr, ptr}),
                                {owner, object});
  lowering_.builder_.CreateBr(doneBlock);
  lowering_.builder_.SetInsertPoint(plainBlock);
  lowering_.builder_.CreateCall(lowering_.plainObjReleaseFn_, {object});
  lowering_.builder_.CreateBr(doneBlock);
  lowering_.builder_.SetInsertPoint(doneBlock);
}

void StoreLowerer::forgetWeakRefs(llvm::Value *object) {
  llvm::Type *i64 = lowering_.builder_.getInt64Ty();
  llvm::Value *count = lowering_.builder_.CreateLoad(
      i64, lowering_.module_->getOrInsertGlobal("tz_weak_count", i64));
  llvm::BasicBlock *forgetBlock = blockHere(lowering_.builder_, "weak.forget");
  llvm::BasicBlock *doneBlock = blockHere(lowering_.builder_, "weak.done");
  lowering_.builder_.CreateCondBr(
      lowering_.builder_.CreateICmpNE(count, lowering_.builder_.getInt64(0)), forgetBlock,
      doneBlock);
  lowering_.builder_.SetInsertPoint(forgetBlock);
  lowering_.builder_.CreateCall(
      lowering_.groupLowerer().groupFunction("tz_weak_forget", lowering_.builder_.getVoidTy(),
                                             {lowering_.builder_.getPtrTy()}),
      {object});
  lowering_.builder_.CreateBr(doneBlock);
  lowering_.builder_.SetInsertPoint(doneBlock);
}

void StoreLowerer::emitMemberDrop(const Type *type, Lowering::ObjectLayout &layout) {
  llvm::Function *drop = layout.drop;
  llvm::Function *saved = lowering_.function_;
  lowering_.function_ = drop;
  lowering_.builder_.SetInsertPoint(llvm::BasicBlock::Create(lowering_.ctx_, "entry", drop));
  llvm::Value *object = drop->getArg(0);
  if (lowering_.isWeakTarget(type)) {
    forgetWeakRefs(object);
  }
  llvm::ArrayRef<Field> fields = type->fields();
  for (unsigned i = 0; i < fields.size(); ++i) {
    const Type *fieldType = fields[i].type;
    if (!fieldType->isManaged()) {
      continue;
    }
    llvm::Value *address =
        lowering_.builder_.CreateStructGEP(layout.type, object, layout.elements[i]);
    llvm::Value *value = lowering_.builder_.CreateLoad(lowering_.moduleBuilder().lower(fieldType),
                                                       address, fields[i].name);
    lowering_.memoryDebug().checkOwned(type, fields[i].name, value, fieldType);
    releaseIn(object, value, fieldType);
  }
  if (const ClassDecl *cls = type->classDecl(); cls && cls->group) {
    const std::vector<VarSymbol *> &captures = cls->group->captures;
    for (size_t i = 0; i < captures.size(); ++i) {
      const VarSymbol *var = captures[i];
      llvm::Value *address =
          lowering_.builder_.CreateStructGEP(layout.type, object, unsigned(layout.captures + i));
      if (var->boxed()) {
        releaseObjectIn(object, lowering_.builder_.CreateLoad(lowering_.builder_.getPtrTy(),
                                                              address, var->name + ".box"));
      } else if (ClosureLowerer::typeOf(var)->isManaged()) {
        const Type *varType = ClosureLowerer::typeOf(var);
        releaseIn(object,
                  lowering_.builder_.CreateLoad(lowering_.moduleBuilder().lower(varType), address,
                                                var->name),
                  varType);
      }
    }
  }
  lowering_.builder_.CreateCall(
      lowering_.groupLowerer().groupFunction("tz_group_obj_free", lowering_.builder_.getVoidTy(),
                                             {lowering_.builder_.getPtrTy()}),
      {object});
  lowering_.builder_.CreateRetVoid();
  lowering_.function_ = saved;
}

}
