#include "compiler/lower/errors/try_lowerer/try_lowerer.h"
#include "compiler/lower/module_builder/module_builder.h"
#include "compiler/lower/function_frame/function_frame.h"
#include "compiler/lower/memory/ref_counts/ref_counts.h"
#include "compiler/lower/types/optional_lowerer/optional_lowerer.h"
#include "compiler/lower/stmt/stmt_lowerer/stmt_lowerer.h"
#include "compiler/lower/async/async_lowerer/async_lowerer.h"
#include "compiler/lower/memory/store_lowerer/store_lowerer.h"
#include "compiler/lower/iterators/coroutine_lowerer/coroutine_lowerer.h"
#include "compiler/lower/errors/throw_lowerer/throw_lowerer.h"
#include "compiler/lower/errors/unwind_lowerer/unwind_lowerer.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/IR/MDBuilder.h"
#include "llvm/Support/ConvertUTF.h"
#include "llvm/Support/FileSystem.h"
#include "llvm/Support/Path.h"
#include <functional>
#include <map>
#include <optional>

using llvm::cast;
using llvm::dyn_cast;
using llvm::isa;
using llvm::BasicBlock;
using llvm::Value;

namespace tz {

// What a function returns: the value of its `return`, or for a generator what
// its iterator gives when it is done.
static const Type *returnedType(const FunctionDecl *fn, const Type *returnType) {
  if (fn && fn->isGenerator && fn->sym && fn->sym->ret && fn->sym->ret->isIterator()) {
    return fn->sym->ret->returned();
  }
  // An async function: what its promise holds.
  if (fn && fn->isAsync && returnType && returnType->isPromise()) {
    return returnType->promised();
  }

  return returnType;
}

TryLowerer::TryLowerer(CodeGen &codegen) : codegen_(codegen) {}

CodeGen::TryContext *TryLowerer::finallyBetween(size_t target) {
  for (CodeGen::TryContext &t : llvm::reverse(codegen_.tries_)) {
    if (target != ~size_t(0) && t.targetsDepth <= target) {
      return nullptr; // the target is inside this `try`, and so inside the rest
    }
    if (t.finally && t.phase != CodeGen::TryContext::Phase::Finally) {
      return &t;
    }
  }

  return nullptr;
}

void TryLowerer::emitJumpTo(size_t target, bool isBreak) {
  if (CodeGen::TryContext *t = finallyBetween(target)) {
    size_t k = 0;
    while (k < t->exits.size() &&
           (t->exits[k].target != target || t->exits[k].isBreak != isBreak)) {
      ++k;
    }
    if (k == t->exits.size()) {
      t->exits.push_back({target, isBreak});
    }
    codegen_.builder_.CreateStore(codegen_.builder_.getInt32(uint32_t(3 + k)), t->completion);
    codegen_.functionFrame().emitCleanups(t->scopeDepth);
    codegen_.builder_.CreateBr(t->finally);
    codegen_.functionFrame().startDeadBlock();
    return;
  }
  const CodeGen::JumpTarget &jt = codegen_.targets_[target];
  bool inGenerator = !isBreak && jt.continueDepth != ~size_t(0);
  codegen_.functionFrame().emitCleanups(inGenerator ? jt.continueDepth : jt.scopeDepth);
  codegen_.builder_.CreateBr(isBreak ? jt.breakTarget : jt.continueTarget);
  codegen_.functionFrame().startDeadBlock();
}

void TryLowerer::emitReturnOf(llvm::Value *value) {
  const Type *type = returnedType(codegen_.currentFn_, codegen_.returnType_);
  if (CodeGen::TryContext *t = finallyBetween(~size_t(0))) {
    if (value) {
      if (!t->pendingValue) {
        // Empty until a `return` fills it (see emitTry).
        llvm::Type *lowered = codegen_.moduleBuilder().lower(type);
        llvm::AllocaInst *slot = codegen_.functionFrame().createStackSlot(lowered, "pending.value");
        llvm::IRBuilder<> init(slot->getParent(), std::next(slot->getIterator()));
        init.CreateStore(llvm::Constant::getNullValue(lowered), slot);
        t->pendingValue = slot;
      }
      codegen_.builder_.CreateStore(value, t->pendingValue);
    }
    t->returns = true;
    codegen_.builder_.CreateStore(codegen_.builder_.getInt32(2), t->completion);
    codegen_.functionFrame().emitCleanups(t->scopeDepth);
    codegen_.builder_.CreateBr(t->finally);
    codegen_.functionFrame().startDeadBlock();
    return;
  }
  if (codegen_.coro_ && codegen_.currentFn_ && codegen_.currentFn_->isGenerator) {
    codegen_.coroutineLowerer().emitCoroutineReturn(value, value ? type : nullptr);
    return;
  }
  if (codegen_.async_) {
    codegen_.asyncLowerer().emitAsyncReturn(value);
    return;
  }
  codegen_.functionFrame().emitCleanups(0);
  if (value) {
    codegen_.builder_.CreateRet(value);
  } else if (codegen_.returnType_ && codegen_.returnType_->isOptional()) {
    codegen_.builder_.CreateRet(codegen_.optionalLowerer().undefinedValue(codegen_.returnType_)); // `return;`
  } else {
    codegen_.builder_.CreateRetVoid();
  }
  codegen_.functionFrame().startDeadBlock();
}

// The block runs; an error from it goes to the `catch`, which takes it from
// tz_thrown, or with only a `finally`, to that. The `finally` runs after the
// block and the `catch`, however they end: `completion` says how to go on.
// While it runs, what is pending (the error to throw again, the value to
// return) waits in slots of its scope, which a way out of the `finally` of
// its own (a return, a jump, an error) releases.
void TryLowerer::emitTry(TryStmt *stmt) {
  size_t depth = codegen_.scopes_.size();
  {
    CodeGen::TryContext t;
    t.stmt = stmt;
    t.scopeDepth = depth;
    t.targetsDepth = codegen_.targets_.size();
    if (stmt->handler) {
      t.handler = codegen_.functionFrame().newBlock("catch");
    }
    if (stmt->finalizer) {
      t.finally = codegen_.functionFrame().newBlock("finally");
      t.throwEntry = codegen_.functionFrame().newBlock("finally.throw");
      t.completion = codegen_.functionFrame().createStackSlot(codegen_.builder_.getInt32Ty(), "completion");
      llvm::AllocaInst *slot = codegen_.functionFrame().createStackSlot(codegen_.builder_.getPtrTy(), "pending.error");
      llvm::IRBuilder<> init(slot->getParent(), std::next(slot->getIterator()));
      init.CreateStore(llvm::ConstantPointerNull::get(codegen_.builder_.getPtrTy()), slot);
      t.pendingError = slot;
    }
    codegen_.tries_.push_back(std::move(t));
  }
  size_t index = codegen_.tries_.size() - 1;
  llvm::BasicBlock *endBlock = codegen_.functionFrame().newBlock("try.end");
  // The block or the `catch` ended normally.
  auto leave = [&] {
    if (!codegen_.functionFrame().blockIsOpen()) {
      return;
    }
    if (stmt->finalizer) {
      codegen_.builder_.CreateStore(codegen_.builder_.getInt32(0), codegen_.tries_[index].completion);
      codegen_.builder_.CreateBr(codegen_.tries_[index].finally);
    } else {
      codegen_.builder_.CreateBr(endBlock);
    }
  };
  // Takes the error in flight.
  auto takeError = [&] {
    llvm::Value *error = codegen_.builder_.CreateLoad(codegen_.builder_.getPtrTy(), codegen_.throwLowerer().thrownSlot(), "error");
    codegen_.builder_.CreateStore(llvm::ConstantPointerNull::get(codegen_.builder_.getPtrTy()), codegen_.throwLowerer().thrownSlot());

    return error;
  };

  codegen_.stmtLowerer().emitStmt(stmt->block);
  leave();

  if (stmt->handler) {
    codegen_.tries_[index].phase = CodeGen::TryContext::Phase::Handler;
    codegen_.functionFrame().switchTo(codegen_.tries_[index].handler);
    llvm::Value *error = takeError();
    codegen_.functionFrame().pushScope();
    if (const VarSymbol *var = stmt->param.sym) {
      codegen_.storeLowerer().storeVar(var, codegen_.functionFrame().createLocal(var, stmt->param.name), error, /*replacing=*/false);
    } else {
      codegen_.refCounts().releaseValue(error, codegen_.errorClass_->type);
    }
    codegen_.stmtLowerer().emitStmt(stmt->handler);
    codegen_.functionFrame().popScope();
    leave();
  }

  if (stmt->finalizer) {
    CodeGen::TryContext *t = &codegen_.tries_[index];
    t->phase = CodeGen::TryContext::Phase::Finally;
    codegen_.functionFrame().switchTo(t->throwEntry);
    codegen_.builder_.CreateStore(takeError(), t->pendingError);
    codegen_.builder_.CreateStore(codegen_.builder_.getInt32(1), t->completion);
    codegen_.builder_.CreateBr(t->finally);

    codegen_.functionFrame().switchTo(t->finally);
    codegen_.functionFrame().pushScope();
    const Type *valueType = returnedType(codegen_.currentFn_, codegen_.returnType_);
    codegen_.scopes_.back().slots.push_back({t->pendingError, codegen_.errorClass_->type});
    if (t->pendingValue && valueType->isManaged()) {
      codegen_.scopes_.back().slots.push_back({t->pendingValue, valueType});
    }
    codegen_.stmtLowerer().emitStmt(stmt->finalizer);
    t = &codegen_.tries_[index];
    if (!codegen_.functionFrame().blockIsOpen()) {
      codegen_.functionFrame().popScope();
    } else {
      // Ended normally: go on as the block or the `catch` did.
      llvm::Value *completion =
          codegen_.builder_.CreateLoad(codegen_.builder_.getInt32Ty(), t->completion, "completion");
      llvm::Value *error = codegen_.builder_.CreateLoad(codegen_.builder_.getPtrTy(), t->pendingError);
      codegen_.builder_.CreateStore(llvm::ConstantPointerNull::get(codegen_.builder_.getPtrTy()), t->pendingError);
      llvm::Value *value = nullptr;
      if (t->pendingValue) {
        llvm::Type *lowered = codegen_.moduleBuilder().lower(valueType);
        value = codegen_.builder_.CreateLoad(lowered, t->pendingValue);
        codegen_.builder_.CreateStore(llvm::Constant::getNullValue(lowered), t->pendingValue);
      }
      codegen_.functionFrame().popScope();
      std::vector<CodeGen::TryContext::Exit> exits = codegen_.tries_[index].exits;
      bool returns = codegen_.tries_[index].returns;
      llvm::BasicBlock *rethrowBlock = codegen_.functionFrame().newBlock("finally.rethrow");
      llvm::BasicBlock *returnBlock = returns ? codegen_.functionFrame().newBlock("finally.return") : nullptr;
      llvm::SwitchInst *dispatch =
          codegen_.builder_.CreateSwitch(completion, endBlock, unsigned(exits.size() + 2));
      dispatch->addCase(codegen_.builder_.getInt32(1), rethrowBlock);
      if (returns) {
        dispatch->addCase(codegen_.builder_.getInt32(2), returnBlock);
      }
      std::vector<llvm::BasicBlock *> exitBlocks;
      for (size_t k = 0; k < exits.size(); ++k) {
        exitBlocks.push_back(codegen_.functionFrame().newBlock("finally.exit"));
        dispatch->addCase(codegen_.builder_.getInt32(uint32_t(3 + k)), exitBlocks.back());
      }
      // The error goes on up, its places kept.
      codegen_.functionFrame().switchTo(rethrowBlock);
      codegen_.builder_.CreateStore(error, codegen_.throwLowerer().thrownSlot());
      codegen_.builder_.CreateBr(codegen_.unwindLowerer().unwindTarget());
      if (returns) {
        codegen_.functionFrame().switchTo(returnBlock);
        emitReturnOf(value);
        codegen_.builder_.CreateUnreachable(); // ends the dead block after it
      }
      for (size_t k = 0; k < exits.size(); ++k) {
        codegen_.functionFrame().switchTo(exitBlocks[k]);
        emitJumpTo(exits[k].target, exits[k].isBreak);
        codegen_.builder_.CreateUnreachable();
      }
    }
  }
  codegen_.tries_.pop_back();
  codegen_.functionFrame().switchTo(endBlock);
}

} // namespace tz
