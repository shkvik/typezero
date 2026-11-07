#include "compiler/lower/function_frame/function_frame.h"
#include "compiler/library/process/process_lowerer/process_lowerer.h"
#include "compiler/lower/debug_info/debug_info.h"
#include "compiler/lower/functions/closure_lowerer/closure_lowerer.h"
#include "compiler/lower/functions/env_lowerer/env_lowerer.h"
#include "compiler/lower/iterators/async_generator_lowerer/async_generator_lowerer.h"
#include "compiler/lower/iterators/generator_lowerer/generator_lowerer.h"
#include "compiler/lower/memory/publish_lowerer/publish_lowerer.h"
#include "compiler/lower/memory/ref_counts/ref_counts.h"
#include "compiler/lower/module_builder/module_builder.h"
#include "compiler/lower/objects/dict_lowerer/dict_lowerer.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/IR/Intrinsics.h"
#include "llvm/IR/MDBuilder.h"


namespace tz {

FunctionFrame::FunctionFrame(Lowering &owner)
    : lowering(owner) {}

llvm::BasicBlock *FunctionFrame::newBlock(const llvm::Twine &name) {
  return llvm::BasicBlock::Create(this->lowering.ctx_, name);
}

void FunctionFrame::switchTo(llvm::BasicBlock *block) {
  this->lowering.function_->insert(this->lowering.function_->end(), block);
  this->lowering.builder_.SetInsertPoint(block);
}

void FunctionFrame::emitBlock(llvm::BasicBlock *block) {
  this->branchTo(block);
  this->switchTo(block);
}

bool FunctionFrame::blockIsOpen() {
  return !this->lowering.builder_.GetInsertBlock()->hasTerminator();
}

void FunctionFrame::branchTo(llvm::BasicBlock *block) {
  if (this->blockIsOpen()) {
    this->lowering.builder_.CreateBr(block);
  }
}

void FunctionFrame::startDeadBlock() {
  this->switchTo(this->newBlock("dead"));
}

void FunctionFrame::pushScope() {
  this->lowering.scopes_.emplace_back();
}

void FunctionFrame::popScope() {
  if (this->blockIsOpen()) {
    this->emitScopeCleanup(this->lowering.scopes_.back());
  }
  this->lowering.scopes_.pop_back();
}

void FunctionFrame::emitCleanups(size_t depth) {
  for (size_t i = this->lowering.scopes_.size(); i > depth; --i) {
    this->emitScopeCleanup(this->lowering.scopes_[i - 1]);
  }
}

void FunctionFrame::emitScopeCleanup(const Lowering::Scope &scope) {
  for (llvm::Value *slot : llvm::reverse(scope.dicts)) {
    llvm::Value *map =
        this->lowering.builder_.CreateLoad(this->lowering.builder_.getPtrTy(), slot, "map");
    llvm::BasicBlock *endBlock = this->newBlock("dict.iterated");
    llvm::BasicBlock *nextBlock = this->newBlock("dict.iteration.end");
    this->lowering.builder_.CreateCondBr(this->lowering.builder_.CreateIsNotNull(map), endBlock,
                                         nextBlock);
    this->switchTo(endBlock);
    llvm::Value *count = this->lowering.dictLowerer().dictField(map, 8);
    this->lowering.builder_.CreateStore(
        this->lowering.builder_.CreateSub(
            this->lowering.builder_.CreateLoad(this->lowering.builder_.getInt32Ty(), count),
            this->lowering.builder_.getInt32(1)),
        count);
    this->lowering.builder_.CreateBr(nextBlock);
    this->switchTo(nextBlock);
  }
  for (llvm::Value *slot : llvm::reverse(scope.asyncIterators)) {
    this->lowering.asyncGeneratorLowerer().closeAsyncIterator(slot);
  }
  for (const auto &[slot, type] : llvm::reverse(scope.slots)) {
    if (!type) {
      llvm::Value *object =
          this->lowering.builder_.CreateLoad(this->lowering.builder_.getPtrTy(), slot);
      this->lowering.builder_.CreateStore(
          llvm::ConstantPointerNull::get(this->lowering.builder_.getPtrTy()), slot);
      this->lowering.builder_.CreateCall(this->lowering.objReleaseFn_, {object});
      continue;
    }
    llvm::Type *lowered = this->lowering.moduleBuilder().lower(type);
    llvm::Value *value = this->lowering.builder_.CreateLoad(lowered, slot);
    this->lowering.builder_.CreateStore(llvm::Constant::getNullValue(lowered), slot);
    if (this->lowering.eternalSlots_.contains(slot)) {
      this->lowering.publishLowerer().endValue(value, type);
    } else {
      this->lowering.refCounts().releaseValue(value, type);
    }
  }
}

llvm::AllocaInst *FunctionFrame::createStackSlot(llvm::Type *type, const llvm::Twine &name) {
  llvm::BasicBlock &entry = this->lowering.function_->getEntryBlock();
  llvm::IRBuilder<> entryBuilder(&entry, entry.begin());

  return entryBuilder.CreateAlloca(type, nullptr, name);
}

llvm::Value *FunctionFrame::createManagedSlot(const Type *type, const llvm::Twine &name) {
  llvm::Type *lowered = this->lowering.moduleBuilder().lower(type);
  llvm::AllocaInst *slot = this->createStackSlot(lowered, name);
  llvm::IRBuilder<> init(slot->getParent(), std::next(slot->getIterator()));
  init.CreateStore(llvm::Constant::getNullValue(lowered), slot);
  this->lowering.scopes_.back().slots.push_back({slot, type});

  return slot;
}

llvm::Value *FunctionFrame::createObjectSlot(const llvm::Twine &name) {
  llvm::AllocaInst *slot = this->createStackSlot(this->lowering.builder_.getPtrTy(), name);
  llvm::IRBuilder<> init(slot->getParent(), std::next(slot->getIterator()));
  init.CreateStore(llvm::ConstantPointerNull::get(this->lowering.builder_.getPtrTy()), slot);
  this->lowering.scopes_.back().slots.push_back({slot, nullptr});

  return slot;
}

llvm::Value *FunctionFrame::createLocal(const VarSymbol *var, llvm::StringRef name) {
  if (var->boxed()) {
    llvm::Value *slot = this->createObjectSlot(name + ".box");
    this->lowering.boxSlots_[var] = slot;
    llvm::Value *box = this->lowering.envLowerer().newBox(ClosureLowerer::typeOf(var));
    llvm::Value *old = this->lowering.builder_.CreateLoad(this->lowering.builder_.getPtrTy(), slot);
    this->lowering.builder_.CreateStore(box, slot);
    this->lowering.builder_.CreateCall(this->lowering.objReleaseFn_, {old});
    llvm::Value *value = this->lowering.builder_.CreateStructGEP(
        this->lowering.envLowerer().boxType(ClosureLowerer::typeOf(var)), box, 2, name);
    this->lowering.debugInfo().declare(var, value);
    return value;
  }
  llvm::Value *slot =
      var->type->isManaged()
          ? this->createManagedSlot(var->type, name)
          : this->createStackSlot(this->lowering.moduleBuilder().lower(var->type), name);
  this->lowering.addresses_[var] = slot;
  this->lowering.debugInfo().declare(var, slot);

  return slot;
}

llvm::Value *FunctionFrame::addressOf(const VarSymbol *var) {
  if (var->boxed()) {
    return this->lowering.builder_.CreateStructGEP(
        this->lowering.envLowerer().boxType(ClosureLowerer::typeOf(var)),
        this->lowering.envLowerer().boxPointer(var), 2);
  }
  llvm::Value *address = this->lowering.addresses_.lookup(var);
  if (address) {
    return address;
  }
  llvm::Value *global = this->lowering.globals_.lookup(var);
  assert(global && "variable used before its declaration was emitted");
  if (this->lowering.parallel_ && this->lowering.function_ &&
      this->lowering.libraryState_.contains(var)) {
    this->lowering.builder_.CreateCall(this->lowering.processLowerer().processFunction(
        "tz_order_enter", this->lowering.builder_.getVoidTy(), {}));
  }

  return global;
}

llvm::Value *FunctionFrame::spill(llvm::Value *value, const llvm::Twine &name) {
  llvm::AllocaInst *slot = this->createStackSlot(value->getType(), name);
  this->lowering.builder_.CreateStore(value, slot);

  return slot;
}

}
