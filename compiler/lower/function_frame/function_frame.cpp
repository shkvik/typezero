#include "compiler/lower/function_frame/function_frame.h"
#include "compiler/library/process/process_lowerer/process_lowerer.h"
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
#include "llvm/Support/ConvertUTF.h"
#include "llvm/Support/ErrorHandling.h"
#include <array>

using llvm::BasicBlock;
using llvm::cast;
using llvm::dyn_cast;
using llvm::isa;
using llvm::Value;

namespace tz {

FunctionFrame::FunctionFrame(Lowering &lowering) : lowering_(lowering) {}

llvm::BasicBlock *FunctionFrame::newBlock(const llvm::Twine &name) {
  return llvm::BasicBlock::Create(lowering_.ctx_, name);
}

void FunctionFrame::switchTo(llvm::BasicBlock *block) {
  lowering_.function_->insert(lowering_.function_->end(), block);
  lowering_.builder_.SetInsertPoint(block);
}

void FunctionFrame::emitBlock(llvm::BasicBlock *block) {
  branchTo(block);
  switchTo(block);
}

bool FunctionFrame::blockIsOpen() { return !lowering_.builder_.GetInsertBlock()->hasTerminator(); }

void FunctionFrame::branchTo(llvm::BasicBlock *block) {
  if (blockIsOpen()) {
    lowering_.builder_.CreateBr(block);
  }
}

void FunctionFrame::startDeadBlock() { switchTo(newBlock("dead")); }

void FunctionFrame::pushScope() { lowering_.scopes_.emplace_back(); }

void FunctionFrame::popScope() {
  if (blockIsOpen()) {
    emitScopeCleanup(lowering_.scopes_.back());
  }
  lowering_.scopes_.pop_back();
}

void FunctionFrame::emitCleanups(size_t depth) {
  for (size_t i = lowering_.scopes_.size(); i > depth; --i) {
    emitScopeCleanup(lowering_.scopes_[i - 1]);
  }
}

void FunctionFrame::emitScopeCleanup(const Lowering::Scope &scope) {
  for (llvm::Value *slot : llvm::reverse(scope.dicts)) {
    llvm::Value *map = lowering_.builder_.CreateLoad(lowering_.builder_.getPtrTy(), slot, "map");
    llvm::BasicBlock *endBlock = newBlock("dict.iterated");
    llvm::BasicBlock *nextBlock = newBlock("dict.iteration.end");
    lowering_.builder_.CreateCondBr(lowering_.builder_.CreateIsNotNull(map), endBlock, nextBlock);
    switchTo(endBlock);
    llvm::Value *count = lowering_.dictLowerer().dictField(map, 8);
    lowering_.builder_.CreateStore(
        lowering_.builder_.CreateSub(
            lowering_.builder_.CreateLoad(lowering_.builder_.getInt32Ty(), count),
            lowering_.builder_.getInt32(1)),
        count);
    lowering_.builder_.CreateBr(nextBlock);
    switchTo(nextBlock);
  }
  for (llvm::Value *slot : llvm::reverse(scope.asyncIterators)) {
    lowering_.asyncGeneratorLowerer().closeAsyncIterator(slot);
  }
  for (llvm::Value *slot : llvm::reverse(scope.iterators)) {
    llvm::Value *it =
        lowering_.builder_.CreateLoad(lowering_.builder_.getPtrTy(), slot, "iterator");
    llvm::BasicBlock *closeBlock = newBlock("iter.close");
    llvm::BasicBlock *nextBlock = newBlock("iter.closed");
    lowering_.builder_.CreateCondBr(lowering_.builder_.CreateIsNotNull(it), closeBlock, nextBlock);
    switchTo(closeBlock);
    lowering_.builder_.CreateCall(lowering_.generatorLowerer().iterCloseFunction(), {it});
    lowering_.builder_.CreateBr(nextBlock);
    switchTo(nextBlock);
  }
  for (const auto &[slot, type] : llvm::reverse(scope.slots)) {
    if (!type) {
      llvm::Value *object = lowering_.builder_.CreateLoad(lowering_.builder_.getPtrTy(), slot);
      lowering_.builder_.CreateStore(llvm::ConstantPointerNull::get(lowering_.builder_.getPtrTy()),
                                     slot);
      lowering_.builder_.CreateCall(lowering_.objReleaseFn_, {object});
      continue;
    }
    llvm::Type *lowered = lowering_.moduleBuilder().lower(type);
    llvm::Value *value = lowering_.builder_.CreateLoad(lowered, slot);
    lowering_.builder_.CreateStore(llvm::Constant::getNullValue(lowered), slot);
    if (lowering_.eternalSlots_.contains(slot)) {
      lowering_.publishLowerer().endValue(value, type);
    } else {
      lowering_.refCounts().releaseValue(value, type);
    }
  }
}

llvm::AllocaInst *FunctionFrame::createStackSlot(llvm::Type *type, const llvm::Twine &name) {
  llvm::BasicBlock &entry = lowering_.function_->getEntryBlock();
  llvm::IRBuilder<> entryBuilder(&entry, entry.begin());

  return entryBuilder.CreateAlloca(type, nullptr, name);
}

llvm::Value *FunctionFrame::createManagedSlot(const Type *type, const llvm::Twine &name) {
  llvm::Type *lowered = lowering_.moduleBuilder().lower(type);
  llvm::AllocaInst *slot = createStackSlot(lowered, name);
  llvm::IRBuilder<> init(slot->getParent(), std::next(slot->getIterator()));
  init.CreateStore(llvm::Constant::getNullValue(lowered), slot);
  lowering_.scopes_.back().slots.push_back({slot, type});

  return slot;
}

llvm::Value *FunctionFrame::createObjectSlot(const llvm::Twine &name) {
  llvm::AllocaInst *slot = createStackSlot(lowering_.builder_.getPtrTy(), name);
  llvm::IRBuilder<> init(slot->getParent(), std::next(slot->getIterator()));
  init.CreateStore(llvm::ConstantPointerNull::get(lowering_.builder_.getPtrTy()), slot);
  lowering_.scopes_.back().slots.push_back({slot, nullptr});

  return slot;
}

llvm::Value *FunctionFrame::createLocal(const VarSymbol *var, llvm::StringRef name) {
  if (var->boxed()) {
    llvm::Value *slot = createObjectSlot(name + ".box");
    lowering_.boxSlots_[var] = slot;
    llvm::Value *box = lowering_.envLowerer().newBox(ClosureLowerer::typeOf(var));
    llvm::Value *old = lowering_.builder_.CreateLoad(lowering_.builder_.getPtrTy(), slot);
    lowering_.builder_.CreateStore(box, slot);
    lowering_.builder_.CreateCall(lowering_.objReleaseFn_, {old});
    return lowering_.builder_.CreateStructGEP(
        lowering_.envLowerer().boxType(ClosureLowerer::typeOf(var)), box, 2, name);
  }
  llvm::Value *slot = var->type->isManaged()
                          ? createManagedSlot(var->type, name)
                          : createStackSlot(lowering_.moduleBuilder().lower(var->type), name);
  lowering_.addresses_[var] = slot;

  return slot;
}

llvm::Value *FunctionFrame::addressOf(const VarSymbol *var) {
  if (var->boxed()) {
    return lowering_.builder_.CreateStructGEP(
        lowering_.envLowerer().boxType(ClosureLowerer::typeOf(var)),
        lowering_.envLowerer().boxPointer(var), 2);
  }
  if (llvm::Value *address = lowering_.addresses_.lookup(var)) {
    return address;
  }
  llvm::Value *global = lowering_.globals_.lookup(var);
  assert(global && "variable used before its declaration was emitted");
  if (lowering_.parallel_ && lowering_.function_ && lowering_.libraryState_.contains(var)) {
    lowering_.builder_.CreateCall(lowering_.processLowerer().processFunction(
        "tz_order_enter", lowering_.builder_.getVoidTy(), {}));
  }

  return global;
}

llvm::Value *FunctionFrame::spill(llvm::Value *value, const llvm::Twine &name) {
  llvm::AllocaInst *slot = createStackSlot(value->getType(), name);
  lowering_.builder_.CreateStore(value, slot);

  return slot;
}

}
