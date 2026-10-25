#include "compiler/lower/memory/store_lowerer/store_lowerer.h"
#include "compiler/lower/module_builder/module_builder.h"
#include "compiler/lower/function_frame/function_frame.h"
#include "compiler/lower/memory/ref_counts/ref_counts.h"
#include "compiler/lower/types/optional_lowerer/optional_lowerer.h"
#include "compiler/lower/stmt/stmt_lowerer/stmt_lowerer.h"
#include "compiler/lower/layouts/type_descriptors/type_descriptors.h"
#include "compiler/lower/functions/closure_lowerer/closure_lowerer.h"
#include "compiler/lower/functions/env_lowerer/env_lowerer.h"
#include "compiler/lower/memory/memory_analysis/memory_analysis.h"
#include "compiler/lower/memory/group_lowerer/group_lowerer.h"
#include "compiler/lower/memory/memory_debug/memory_debug.h"
#include "compiler/library/process/process_lowerer/process_lowerer.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/IR/MDBuilder.h"
#include "llvm/Support/ConvertUTF.h"
#include <functional>
#include <map>
#include <set>

using llvm::cast;
using llvm::dyn_cast;
using llvm::isa;
using llvm::BasicBlock;
using llvm::Value;

namespace tz {

namespace {

// The code of a group function may land in a drop function that is emitted
// while another one is (objectLayout makes them when first needed): its
// blocks and stack slots go to the function the builder is in.
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

// How CodeGen walks the references in a value for the group functions: the
// reference itself, or the environment of a function; anything else goes to
// the runtime by its descriptor.
enum class RefShape { None, Pointer, Function, Optional, Other };

} // namespace

static RefShape refShape(const Type *type) {
  switch (type->kind()) {
  case Type::Kind::Object:
  case Type::Kind::Array:
  case Type::Kind::Dict:
  case Type::Kind::Unknown:
  case Type::Kind::Promise: // which may be a member (promiseIsMember)
    return RefShape::Pointer;
  case Type::Kind::Function:
    return RefShape::Function;
  case Type::Kind::Union:
    return type->isTagged() ? RefShape::Other : RefShape::Pointer; // one of objects
  case Type::Kind::Optional:
    return RefShape::Optional;
  case Type::Kind::Result:
    return RefShape::Other;
  default:
    return RefShape::None;
  }
}

StoreLowerer::StoreLowerer(CodeGen &codegen) : codegen_(codegen) {}

void StoreLowerer::groupStore(llvm::Value *owner, llvm::Value *value, const Type *type) {
  if (!codegen_.memoryAnalysis().mayHoldMember(type) || isa<llvm::Constant>(value)) {
    return;
  }
  llvm::Type *ptr = codegen_.builder_.getPtrTy();
  // Debug mode notes where the store is, for what a walk reports.
  if (codegen_.debugMemory_) {
    // Each thread's own in a parallel program (runtime/core/group/group.c).
    auto *site = cast<llvm::GlobalVariable>(codegen_.module_->getOrInsertGlobal("tz_group_site", ptr));
    site->setThreadLocal(codegen_.parallel_);
    codegen_.builder_.CreateStore(
        codegen_.groupLowerer().groupSite(),
        codegen_.parallel_ ? static_cast<llvm::Value *>(codegen_.builder_.CreateThreadLocalAddress(site)) : site);
  }
  switch (refShape(type)) {
  case RefShape::Pointer:
    codegen_.builder_.CreateCall(codegen_.groupLowerer().groupFunction("tz_group_store", codegen_.builder_.getVoidTy(), {ptr, ptr}),
                        {owner, value});
    return;
  case RefShape::Function:
    codegen_.builder_.CreateCall(codegen_.groupLowerer().groupFunction("tz_group_store", codegen_.builder_.getVoidTy(), {ptr, ptr}),
                        {owner, codegen_.builder_.CreateExtractValue(value, 1)});
    return;
  case RefShape::Optional:
    // Undefined holds zero, which refers to nothing.
    groupStore(owner, codegen_.optionalLowerer().optionalValue(value, type), type->inner());
    return;
  case RefShape::Other:
    codegen_.builder_.CreateCall(
        codegen_.groupLowerer().groupFunction("tz_group_store_value", codegen_.builder_.getVoidTy(), {ptr, ptr, ptr}),
        {owner, codegen_.typeDescriptors().typeDescriptor(type), spillHere(codegen_.builder_, value, "stored")});
    return;
  case RefShape::None:
    return;
  }
}

void StoreLowerer::releaseIn(llvm::Value *owner, llvm::Value *value, const Type *type) {
  if (!codegen_.memoryAnalysis().mayHoldMember(type) || isa<llvm::Constant>(value)) {
    codegen_.refCounts().releaseValue(value, type);
    return;
  }
  llvm::Type *ptr = codegen_.builder_.getPtrTy();
  auto viaGroup = [&](llvm::Value *ref, const std::function<void()> &plain) {
    llvm::BasicBlock *refBlock = blockHere(codegen_.builder_, "in.ref");
    llvm::BasicBlock *memberBlock = blockHere(codegen_.builder_, "in.member");
    llvm::BasicBlock *plainBlock = blockHere(codegen_.builder_, "in.plain");
    llvm::BasicBlock *doneBlock = blockHere(codegen_.builder_, "in.done");
    codegen_.builder_.CreateCondBr(codegen_.builder_.CreateIsNotNull(ref), refBlock, doneBlock);
    codegen_.builder_.SetInsertPoint(refBlock);
    llvm::Value *count = codegen_.builder_.CreateLoad(codegen_.builder_.getInt64Ty(), ref, "rc");
    codegen_.builder_.CreateCondBr(
        codegen_.builder_.CreateICmpSLT(count, codegen_.builder_.getInt64(uint64_t(-(int64_t(1) << 60)))),
        memberBlock, plainBlock);
    codegen_.builder_.SetInsertPoint(memberBlock);
    codegen_.builder_.CreateCall(codegen_.groupLowerer().groupFunction("tz_group_unstore", codegen_.builder_.getVoidTy(), {ptr, ptr}),
                        {owner, ref});
    codegen_.builder_.CreateBr(doneBlock);
    codegen_.builder_.SetInsertPoint(plainBlock);
    plain();
    codegen_.builder_.CreateBr(doneBlock);
    codegen_.builder_.SetInsertPoint(doneBlock);
  };
  switch (refShape(type)) {
  case RefShape::Pointer:
    viaGroup(value, [&] { codegen_.refCounts().releaseValue(value, type); });
    return;
  case RefShape::Function:
    viaGroup(codegen_.builder_.CreateExtractValue(value, 1), [&] { codegen_.refCounts().releaseValue(value, type); });
    return;
  case RefShape::Optional:
    releaseIn(owner, codegen_.optionalLowerer().optionalValue(value, type), type->inner());
    return;
  case RefShape::Other:
    codegen_.builder_.CreateCall(codegen_.groupLowerer().groupFunction("tz_value_release_in", codegen_.builder_.getVoidTy(), {ptr, ptr, ptr}),
                        {owner, codegen_.typeDescriptors().typeDescriptor(type), spillHere(codegen_.builder_, value, "released")});
    return;
  case RefShape::None:
    codegen_.refCounts().releaseValue(value, type);
    return;
  }
}

void StoreLowerer::takeFrom(llvm::Value *owner, llvm::Value *value, const Type *type) {
  if (!codegen_.memoryAnalysis().mayHoldMember(type) || isa<llvm::Constant>(value)) {
    return;
  }
  llvm::Type *ptr = codegen_.builder_.getPtrTy();
  auto take = [&](llvm::Value *ref) {
    llvm::BasicBlock *refBlock = blockHere(codegen_.builder_, "take.ref");
    llvm::BasicBlock *memberBlock = blockHere(codegen_.builder_, "take.member");
    llvm::BasicBlock *doneBlock = blockHere(codegen_.builder_, "take.done");
    codegen_.builder_.CreateCondBr(codegen_.builder_.CreateIsNotNull(ref), refBlock, doneBlock);
    codegen_.builder_.SetInsertPoint(refBlock);
    llvm::Value *count = codegen_.builder_.CreateLoad(codegen_.builder_.getInt64Ty(), ref, "rc");
    codegen_.builder_.CreateCondBr(
        codegen_.builder_.CreateICmpSLT(count, codegen_.builder_.getInt64(uint64_t(-(int64_t(1) << 60)))),
        memberBlock, doneBlock);
    codegen_.builder_.SetInsertPoint(memberBlock);
    codegen_.builder_.CreateCall(codegen_.groupLowerer().groupFunction("tz_group_take", codegen_.builder_.getVoidTy(), {ptr, ptr}),
                        {owner, ref});
    codegen_.builder_.CreateBr(doneBlock);
    codegen_.builder_.SetInsertPoint(doneBlock);
  };
  switch (refShape(type)) {
  case RefShape::Pointer:
    take(value);
    return;
  case RefShape::Function:
    take(codegen_.builder_.CreateExtractValue(value, 1));
    return;
  case RefShape::Optional:
    takeFrom(owner, codegen_.optionalLowerer().optionalValue(value, type), type->inner());
    return;
  case RefShape::Other:
    codegen_.builder_.CreateCall(codegen_.groupLowerer().groupFunction("tz_group_take_value", codegen_.builder_.getVoidTy(), {ptr, ptr, ptr}),
                        {owner, codegen_.typeDescriptors().typeDescriptor(type), spillHere(codegen_.builder_, value, "taken")});
    return;
  case RefShape::None:
    return;
  }
}

void StoreLowerer::storeBarrier(llvm::Value *owner, llvm::Value *value, const Type *type) {
  if (!codegen_.parallel_ || !type->isManaged() || isa<llvm::Constant>(value)) {
    return;
  }
  llvm::Type *ptr = codegen_.builder_.getPtrTy();
  llvm::Value *count = codegen_.builder_.CreateLoad(codegen_.builder_.getInt64Ty(), owner, "owner.rc");
  llvm::BasicBlock *barrierBlock = codegen_.functionFrame().newBlock("barrier");
  llvm::BasicBlock *doneBlock = codegen_.functionFrame().newBlock("barrier.done");
  codegen_.builder_.CreateCondBr(codegen_.builder_.CreateICmpSLT(count, codegen_.builder_.getInt64(0)), barrierBlock,
                        doneBlock, llvm::MDBuilder(codegen_.ctx_).createUnlikelyBranchWeights());
  codegen_.functionFrame().switchTo(barrierBlock);
  // An array goes by its type: it has no descriptor of its own.
  RefShape shape = type->isString() || type->isArray() ? RefShape::Other : refShape(type);
  if (shape == RefShape::Pointer || shape == RefShape::Function) {
    llvm::Value *object =
        shape == RefShape::Function ? codegen_.builder_.CreateExtractValue(value, 1) : value;
    codegen_.builder_.CreateCall(codegen_.processLowerer().processFunction("tz_store_barrier_ref", codegen_.builder_.getVoidTy(), {ptr, ptr}),
                        {owner, object});
  } else {
    codegen_.builder_.CreateCall(codegen_.processLowerer().processFunction("tz_store_barrier", codegen_.builder_.getVoidTy(), {ptr, ptr, ptr}),
                        {owner, codegen_.typeDescriptors().typeDescriptor(type), spillHere(codegen_.builder_, value, "published")});
  }
  codegen_.builder_.CreateBr(doneBlock);
  codegen_.functionFrame().switchTo(doneBlock);
}

void StoreLowerer::publishValue(llvm::Value *value, const Type *type) {
  if (!codegen_.parallel_ || !type->isManaged() || isa<llvm::Constant>(value)) {
    return;
  }
  llvm::Type *ptr = codegen_.builder_.getPtrTy();
  codegen_.builder_.CreateCall(codegen_.processLowerer().processFunction("tz_publish_value", codegen_.builder_.getVoidTy(), {ptr, ptr}),
                      {codegen_.typeDescriptors().typeDescriptor(type), spillHere(codegen_.builder_, value, "published")});
}

void StoreLowerer::storeIn(llvm::Value *owner, const Type *ownerType, llvm::Value *address,
                      llvm::Value *value, const Type *type) {
  storeBarrier(owner, value, type);
  if (!type->isManaged() || !codegen_.memoryAnalysis().mayHoldMember(type) || (ownerType && !codegen_.ownerMayBeMember(ownerType))) {
    codegen_.stmtLowerer().storeReplacing(address, value, type);
    return;
  }
  llvm::Value *previous = codegen_.builder_.CreateLoad(codegen_.moduleBuilder().lower(type), address);
  codegen_.builder_.CreateStore(value, address);
  groupStore(owner, value, type);
  releaseIn(owner, previous, type);
}

void StoreLowerer::storeVar(const VarSymbol *var, llvm::Value *address, llvm::Value *value,
                       bool replacing) {
  const Type *type = ClosureLowerer::typeOf(var);
  // What a global holds, functions may read in any task; a box, closures.
  if (var->isGlobal && var->usedInFunction) {
    publishValue(value, type);
  } else if (var->boxed() && !(codegen_.isCyclicBox(type) && codegen_.memoryAnalysis().mayHoldMember(type))) {
    storeBarrier(codegen_.envLowerer().boxPointer(var), value, type);
  }
  if (var->boxed() && codegen_.isCyclicBox(type) && codegen_.memoryAnalysis().mayHoldMember(type)) {
    llvm::Value *box = codegen_.envLowerer().boxPointer(var);
    if (replacing) {
      storeIn(box, nullptr, address, value, type);
    } else {
      codegen_.builder_.CreateStore(value, address);
      groupStore(box, value, type);
    }
    return;
  }
  if (replacing) {
    codegen_.stmtLowerer().storeReplacing(address, value, type);
  } else {
    codegen_.builder_.CreateStore(value, address);
  }
}

void StoreLowerer::releaseObjectIn(llvm::Value *owner, llvm::Value *object) {
  llvm::Type *ptr = codegen_.builder_.getPtrTy();
  llvm::BasicBlock *refBlock = blockHere(codegen_.builder_, "in.ref");
  llvm::BasicBlock *memberBlock = blockHere(codegen_.builder_, "in.member");
  llvm::BasicBlock *plainBlock = blockHere(codegen_.builder_, "in.plain");
  llvm::BasicBlock *doneBlock = blockHere(codegen_.builder_, "in.done");
  codegen_.builder_.CreateCondBr(codegen_.builder_.CreateIsNotNull(object), refBlock, doneBlock);
  codegen_.builder_.SetInsertPoint(refBlock);
  llvm::Value *count = codegen_.builder_.CreateLoad(codegen_.builder_.getInt64Ty(), object, "rc");
  codegen_.builder_.CreateCondBr(
      codegen_.builder_.CreateICmpSLT(count, codegen_.builder_.getInt64(uint64_t(-(int64_t(1) << 60)))), memberBlock,
      plainBlock);
  codegen_.builder_.SetInsertPoint(memberBlock);
  codegen_.builder_.CreateCall(codegen_.groupLowerer().groupFunction("tz_group_unstore", codegen_.builder_.getVoidTy(), {ptr, ptr}),
                      {owner, object});
  codegen_.builder_.CreateBr(doneBlock);
  codegen_.builder_.SetInsertPoint(plainBlock);
  codegen_.builder_.CreateCall(codegen_.plainObjReleaseFn_, {object});
  codegen_.builder_.CreateBr(doneBlock);
  codegen_.builder_.SetInsertPoint(doneBlock);
}

void StoreLowerer::forgetWeakRefs(llvm::Value *object) {
  llvm::Type *i64 = codegen_.builder_.getInt64Ty();
  llvm::Value *count = codegen_.builder_.CreateLoad(i64, codegen_.module_->getOrInsertGlobal("tz_weak_count", i64));
  llvm::BasicBlock *forgetBlock = blockHere(codegen_.builder_, "weak.forget");
  llvm::BasicBlock *doneBlock = blockHere(codegen_.builder_, "weak.done");
  codegen_.builder_.CreateCondBr(codegen_.builder_.CreateICmpNE(count, codegen_.builder_.getInt64(0)), forgetBlock, doneBlock);
  codegen_.builder_.SetInsertPoint(forgetBlock);
  codegen_.builder_.CreateCall(codegen_.groupLowerer().groupFunction("tz_weak_forget", codegen_.builder_.getVoidTy(), {codegen_.builder_.getPtrTy()}),
                      {object});
  codegen_.builder_.CreateBr(doneBlock);
  codegen_.builder_.SetInsertPoint(doneBlock);
}

// Releases what each field holds as a member lets go of it, then frees the
// object unless it is one: a group frees its members' memory itself, after
// all of them released what they hold (runtime/core/group/group.c).
void StoreLowerer::emitMemberDrop(const Type *type, CodeGen::ObjectLayout &layout) {
  llvm::Function *drop = layout.drop;
  llvm::Function *saved = codegen_.function_;
  codegen_.function_ = drop;
  codegen_.builder_.SetInsertPoint(llvm::BasicBlock::Create(codegen_.ctx_, "entry", drop));
  llvm::Value *object = drop->getArg(0);
  if (codegen_.isWeakTarget(type)) {
    forgetWeakRefs(object);
  }
  llvm::ArrayRef<Field> fields = type->fields();
  for (unsigned i = 0; i < fields.size(); ++i) {
    const Type *fieldType = fields[i].type;
    if (!fieldType->isManaged()) {
      continue;
    }
    llvm::Value *address = codegen_.builder_.CreateStructGEP(layout.type, object, layout.elements[i]);
    llvm::Value *value = codegen_.builder_.CreateLoad(codegen_.moduleBuilder().lower(fieldType), address, fields[i].name);
    codegen_.memoryDebug().checkOwned(type, fields[i].name, value, fieldType);
    releaseIn(object, value, fieldType);
  }
  // The variables a literal's methods captured.
  if (const ClassDecl *cls = type->classDecl(); cls && cls->group) {
    const std::vector<VarSymbol *> &captures = cls->group->captures;
    for (size_t i = 0; i < captures.size(); ++i) {
      const VarSymbol *var = captures[i];
      llvm::Value *address =
          codegen_.builder_.CreateStructGEP(layout.type, object, unsigned(layout.captures + i));
      if (var->boxed()) {
        releaseObjectIn(object,
                        codegen_.builder_.CreateLoad(codegen_.builder_.getPtrTy(), address, var->name + ".box"));
      } else if (ClosureLowerer::typeOf(var)->isManaged()) {
        const Type *varType = ClosureLowerer::typeOf(var);
        releaseIn(object, codegen_.builder_.CreateLoad(codegen_.moduleBuilder().lower(varType), address, var->name), varType);
      }
    }
  }
  codegen_.builder_.CreateCall(
      codegen_.groupLowerer().groupFunction("tz_group_obj_free", codegen_.builder_.getVoidTy(), {codegen_.builder_.getPtrTy()}), {object});
  codegen_.builder_.CreateRetVoid();
  codegen_.function_ = saved;
}

} // namespace tz
