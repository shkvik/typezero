#include "compiler/lower/function_frame/function_frame.h"
#include "compiler/lower/module_builder/module_builder.h"
#include "compiler/lower/memory/ref_counts/ref_counts.h"
#include "compiler/lower/iterators/async_generator_lowerer/async_generator_lowerer.h"
#include "compiler/lower/functions/closure_lowerer/closure_lowerer.h"
#include "compiler/lower/functions/env_lowerer/env_lowerer.h"
#include "compiler/lower/memory/publish_lowerer/publish_lowerer.h"
#include "compiler/lower/objects/dict_lowerer/dict_lowerer.h"
#include "compiler/lower/iterators/generator_lowerer/generator_lowerer.h"
#include "compiler/lower/errors/unwind_lowerer/unwind_lowerer.h"
#include "compiler/library/process/process_lowerer/process_lowerer.h"
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

FunctionFrame::FunctionFrame(CodeGen &codegen) : codegen_(codegen) {}

llvm::BasicBlock *FunctionFrame::newBlock(const llvm::Twine &name) {
  return llvm::BasicBlock::Create(codegen_.ctx_, name);
}

void FunctionFrame::switchTo(llvm::BasicBlock *block) {
  codegen_.function_->insert(codegen_.function_->end(), block);
  codegen_.builder_.SetInsertPoint(block);
}

void FunctionFrame::emitBlock(llvm::BasicBlock *block) {
  branchTo(block);
  switchTo(block);
}

bool FunctionFrame::blockIsOpen() { return !codegen_.builder_.GetInsertBlock()->hasTerminator(); }

void FunctionFrame::branchTo(llvm::BasicBlock *block) {
  if (blockIsOpen()) {
    codegen_.builder_.CreateBr(block);
  }
}

void FunctionFrame::startDeadBlock() { switchTo(newBlock("dead")); }

void FunctionFrame::pushScope() { codegen_.scopes_.emplace_back(); }

void FunctionFrame::popScope() {
  if (blockIsOpen()) {
    emitScopeCleanup(codegen_.scopes_.back());
  }
  codegen_.unwindLowerer().finishScopeUnwind(codegen_.scopes_.size() - 1);
  codegen_.scopes_.pop_back();
}

void FunctionFrame::emitCleanups(size_t depth) {
  for (size_t i = codegen_.scopes_.size(); i > depth; --i) {
    emitScopeCleanup(codegen_.scopes_[i - 1]);
  }
}

void FunctionFrame::emitScopeCleanup(const CodeGen::Scope &scope) {
  // The iterations of dictionaries end, before the slots let them go.
  for (llvm::Value *slot : llvm::reverse(scope.dicts)) {
    llvm::Value *map = codegen_.builder_.CreateLoad(codegen_.builder_.getPtrTy(), slot, "map");
    llvm::BasicBlock *endBlock = newBlock("dict.iterated");
    llvm::BasicBlock *nextBlock = newBlock("dict.iteration.end");
    codegen_.builder_.CreateCondBr(codegen_.builder_.CreateIsNotNull(map), endBlock, nextBlock);
    switchTo(endBlock);
    llvm::Value *count = codegen_.dictLowerer().dictField(map, 8); // TzMap::iterating
    codegen_.builder_.CreateStore(
        codegen_.builder_.CreateSub(codegen_.builder_.CreateLoad(codegen_.builder_.getInt32Ty(), count), codegen_.builder_.getInt32(1)),
        count);
    codegen_.builder_.CreateBr(nextBlock);
    switchTo(nextBlock);
  }
  for (llvm::Value *slot : llvm::reverse(scope.asyncIterators)) {
    codegen_.asyncGeneratorLowerer().closeAsyncIterator(slot);
  }
  for (llvm::Value *slot : llvm::reverse(scope.iterators)) {
    llvm::Value *it = codegen_.builder_.CreateLoad(codegen_.builder_.getPtrTy(), slot, "iterator");
    llvm::BasicBlock *closeBlock = newBlock("iter.close");
    llvm::BasicBlock *nextBlock = newBlock("iter.closed");
    codegen_.builder_.CreateCondBr(codegen_.builder_.CreateIsNotNull(it), closeBlock, nextBlock);
    switchTo(closeBlock);
    codegen_.builder_.CreateCall(codegen_.generatorLowerer().iterCloseFunction(), {it});
    codegen_.builder_.CreateBr(nextBlock);
    switchTo(nextBlock);
  }
  for (const auto &[slot, type] : llvm::reverse(scope.slots)) {
    if (!type) { // a box or an environment
      llvm::Value *object = codegen_.builder_.CreateLoad(codegen_.builder_.getPtrTy(), slot);
      codegen_.builder_.CreateStore(llvm::ConstantPointerNull::get(codegen_.builder_.getPtrTy()), slot);
      codegen_.builder_.CreateCall(codegen_.objReleaseFn_, {object});
      continue;
    }
    llvm::Type *lowered = codegen_.moduleBuilder().lower(type);
    llvm::Value *value = codegen_.builder_.CreateLoad(lowered, slot);
    // Emptied, so that a second cleanup of the same slot (a loop that leaves
    // early on the next iteration) does nothing.
    codegen_.builder_.CreateStore(llvm::Constant::getNullValue(lowered), slot);
    if (codegen_.eternalSlots_.contains(slot)) { // a global of main's, at the end
      codegen_.publishLowerer().endValue(value, type);
    } else {
      codegen_.refCounts().releaseValue(value, type);
    }
  }
}

llvm::AllocaInst *FunctionFrame::createStackSlot(llvm::Type *type, const llvm::Twine &name) {
  llvm::BasicBlock &entry = codegen_.function_->getEntryBlock();
  llvm::IRBuilder<> entryBuilder(&entry, entry.begin());

  return entryBuilder.CreateAlloca(type, nullptr, name);
}

llvm::Value *FunctionFrame::createManagedSlot(const Type *type, const llvm::Twine &name) {
  llvm::Type *lowered = codegen_.moduleBuilder().lower(type);
  llvm::AllocaInst *slot = createStackSlot(lowered, name);
  // Starts empty, so that releasing it before it was assigned is harmless.
  llvm::IRBuilder<> init(slot->getParent(), std::next(slot->getIterator()));
  init.CreateStore(llvm::Constant::getNullValue(lowered), slot);
  codegen_.scopes_.back().slots.push_back({slot, type});

  return slot;
}

llvm::Value *FunctionFrame::createObjectSlot(const llvm::Twine &name) {
  llvm::AllocaInst *slot = createStackSlot(codegen_.builder_.getPtrTy(), name);
  llvm::IRBuilder<> init(slot->getParent(), std::next(slot->getIterator()));
  init.CreateStore(llvm::ConstantPointerNull::get(codegen_.builder_.getPtrTy()), slot);
  codegen_.scopes_.back().slots.push_back({slot, nullptr});

  return slot;
}

llvm::Value *FunctionFrame::createLocal(const VarSymbol *var, llvm::StringRef name) {
  if (var->boxed()) {
    // Closures share the variable: it lives in a box, a new one each time
    // the declaration runs.
    llvm::Value *slot = createObjectSlot(name + ".box");
    codegen_.boxSlots_[var] = slot;
    llvm::Value *box = codegen_.envLowerer().newBox(ClosureLowerer::typeOf(var));
    llvm::Value *old = codegen_.builder_.CreateLoad(codegen_.builder_.getPtrTy(), slot);
    codegen_.builder_.CreateStore(box, slot);
    codegen_.builder_.CreateCall(codegen_.objReleaseFn_, {old});
    return codegen_.builder_.CreateStructGEP(codegen_.envLowerer().boxType(ClosureLowerer::typeOf(var)), box, 2, name);
  }
  llvm::Value *slot = var->type->isManaged() ? createManagedSlot(var->type, name)
                                             : createStackSlot(codegen_.moduleBuilder().lower(var->type), name);
  codegen_.addresses_[var] = slot;

  return slot;
}

llvm::Value *FunctionFrame::addressOf(const VarSymbol *var) {
  if (var->boxed()) {
    return codegen_.builder_.CreateStructGEP(codegen_.envLowerer().boxType(ClosureLowerer::typeOf(var)), codegen_.envLowerer().boxPointer(var), 2);
  }
  if (llvm::Value *address = codegen_.addresses_.lookup(var)) {
    return address;
  }
  llvm::Value *global = codegen_.globals_.lookup(var);
  assert(global && "variable used before its declaration was emitted");
  // The state of a built-in module (fetch's pool of connections, the stdio
  // streams...): code that runs ahead of its turn waits for it first, as the
  // analysis does not walk that code (runtime/core/parallel/parallel.c).
  if (codegen_.parallel_ && codegen_.function_ && codegen_.libraryState_.contains(var)) {
    codegen_.builder_.CreateCall(codegen_.processLowerer().processFunction("tz_order_enter", codegen_.builder_.getVoidTy(), {}));
  }

  return global;
}

llvm::Value *FunctionFrame::spill(llvm::Value *value, const llvm::Twine &name) {
  llvm::AllocaInst *slot = createStackSlot(value->getType(), name);
  codegen_.builder_.CreateStore(value, slot);

  return slot;
}

} // namespace tz
