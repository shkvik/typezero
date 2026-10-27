#include "compiler/lower/stmt/try_lowerer/try_lowerer.h"
#include "compiler/lower/async/async_lowerer/async_lowerer.h"
#include "compiler/lower/function_frame/function_frame.h"
#include "compiler/lower/iterators/coroutine_lowerer/coroutine_lowerer.h"
#include "compiler/lower/module_builder/module_builder.h"
#include "compiler/lower/stmt/stmt_lowerer/stmt_lowerer.h"
#include "compiler/lower/types/optional_lowerer/optional_lowerer.h"
#include "llvm/ADT/STLExtras.h"

namespace tz {

static const Type *returnedType(const FunctionDecl *fn, const Type *returnType) {
  if (fn && fn->isGenerator && fn->sym && fn->sym->ret && fn->sym->ret->isIterator()) {
    return fn->sym->ret->returned();
  }
  if (fn && fn->isAsync && returnType && returnType->isPromise()) {
    return returnType->promised();
  }

  return returnType;
}

TryLowerer::TryLowerer(Lowering &lowering) : lowering_(lowering) {}

Lowering::TryContext *TryLowerer::finallyBetween(size_t target) {
  for (Lowering::TryContext &t : llvm::reverse(lowering_.tries_)) {
    if (target != ~size_t(0) && t.targetsDepth <= target) {
      return nullptr;
    }
    if (!t.inFinally) {
      return &t;
    }
  }

  return nullptr;
}

void TryLowerer::emitJumpTo(size_t target, bool isBreak) {
  if (Lowering::TryContext *t = finallyBetween(target)) {
    size_t k = 0;
    while (k < t->exits.size() &&
           (t->exits[k].target != target || t->exits[k].isBreak != isBreak)) {
      ++k;
    }
    if (k == t->exits.size()) {
      t->exits.push_back({target, isBreak});
    }
    lowering_.builder_.CreateStore(lowering_.builder_.getInt32(uint32_t(3 + k)), t->completion);
    lowering_.functionFrame().emitCleanups(t->scopeDepth);
    lowering_.builder_.CreateBr(t->finally);
    lowering_.functionFrame().startDeadBlock();
    return;
  }
  const Lowering::JumpTarget &jt = lowering_.targets_[target];
  bool inGenerator = !isBreak && jt.continueDepth != ~size_t(0);
  lowering_.functionFrame().emitCleanups(inGenerator ? jt.continueDepth : jt.scopeDepth);
  lowering_.builder_.CreateBr(isBreak ? jt.breakTarget : jt.continueTarget);
  lowering_.functionFrame().startDeadBlock();
}

void TryLowerer::emitReturnOf(llvm::Value *value) {
  const Type *type = returnedType(lowering_.currentFn_, lowering_.returnType_);
  if (Lowering::TryContext *t = finallyBetween(~size_t(0))) {
    if (value) {
      if (!t->pendingValue) {
        llvm::Type *lowered = lowering_.moduleBuilder().lower(type);
        llvm::AllocaInst *slot =
            lowering_.functionFrame().createStackSlot(lowered, "pending.value");
        llvm::IRBuilder<> init(slot->getParent(), std::next(slot->getIterator()));
        init.CreateStore(llvm::Constant::getNullValue(lowered), slot);
        t->pendingValue = slot;
      }
      lowering_.builder_.CreateStore(value, t->pendingValue);
    }
    t->returns = true;
    lowering_.builder_.CreateStore(lowering_.builder_.getInt32(2), t->completion);
    lowering_.functionFrame().emitCleanups(t->scopeDepth);
    lowering_.builder_.CreateBr(t->finally);
    lowering_.functionFrame().startDeadBlock();
    return;
  }
  if (lowering_.coro_ && lowering_.currentFn_ && lowering_.currentFn_->isGenerator) {
    lowering_.coroutineLowerer().emitCoroutineReturn(value, value ? type : nullptr);
    return;
  }
  if (lowering_.async_) {
    lowering_.asyncLowerer().emitAsyncReturn(value);
    return;
  }
  lowering_.functionFrame().emitCleanups(0);
  if (value) {
    lowering_.builder_.CreateRet(value);
  } else if (lowering_.returnType_ && lowering_.returnType_->isOptional()) {
    lowering_.builder_.CreateRet(lowering_.optionalLowerer().undefinedValue(lowering_.returnType_));
  } else {
    lowering_.builder_.CreateRetVoid();
  }
  lowering_.functionFrame().startDeadBlock();
}

void TryLowerer::emitTry(TryStmt *stmt) {
  if (!stmt->finalizer) {
    lowering_.stmtLowerer().emitStmt(stmt->block);
    return;
  }
  {
    Lowering::TryContext t;
    t.stmt = stmt;
    t.scopeDepth = lowering_.scopes_.size();
    t.targetsDepth = lowering_.targets_.size();
    t.finally = lowering_.functionFrame().newBlock("finally");
    t.completion =
        lowering_.functionFrame().createStackSlot(lowering_.builder_.getInt32Ty(), "completion");
    lowering_.tries_.push_back(std::move(t));
  }
  size_t index = lowering_.tries_.size() - 1;
  llvm::BasicBlock *endBlock = lowering_.functionFrame().newBlock("try.end");
  lowering_.stmtLowerer().emitStmt(stmt->block);
  if (lowering_.functionFrame().blockIsOpen()) {
    lowering_.builder_.CreateStore(lowering_.builder_.getInt32(0),
                                   lowering_.tries_[index].completion);
    lowering_.builder_.CreateBr(lowering_.tries_[index].finally);
  }
  Lowering::TryContext *t = &lowering_.tries_[index];
  t->inFinally = true;
  lowering_.functionFrame().switchTo(t->finally);
  lowering_.functionFrame().pushScope();
  const Type *valueType = returnedType(lowering_.currentFn_, lowering_.returnType_);
  if (t->pendingValue && valueType->isManaged()) {
    lowering_.scopes_.back().slots.push_back({t->pendingValue, valueType});
  }
  lowering_.stmtLowerer().emitStmt(stmt->finalizer);
  t = &lowering_.tries_[index];
  if (!lowering_.functionFrame().blockIsOpen()) {
    lowering_.functionFrame().popScope();
  } else {
    llvm::Value *completion =
        lowering_.builder_.CreateLoad(lowering_.builder_.getInt32Ty(), t->completion, "completion");
    llvm::Value *value = nullptr;
    if (t->pendingValue) {
      llvm::Type *lowered = lowering_.moduleBuilder().lower(valueType);
      value = lowering_.builder_.CreateLoad(lowered, t->pendingValue);
      lowering_.builder_.CreateStore(llvm::Constant::getNullValue(lowered), t->pendingValue);
    }
    lowering_.functionFrame().popScope();
    std::vector<Lowering::TryContext::Exit> exits = lowering_.tries_[index].exits;
    bool returns = lowering_.tries_[index].returns;
    llvm::BasicBlock *returnBlock =
        returns ? lowering_.functionFrame().newBlock("finally.return") : nullptr;
    llvm::SwitchInst *dispatch =
        lowering_.builder_.CreateSwitch(completion, endBlock, unsigned(exits.size() + 1));
    if (returns) {
      dispatch->addCase(lowering_.builder_.getInt32(2), returnBlock);
    }
    std::vector<llvm::BasicBlock *> exitBlocks;
    for (size_t k = 0; k < exits.size(); ++k) {
      exitBlocks.push_back(lowering_.functionFrame().newBlock("finally.exit"));
      dispatch->addCase(lowering_.builder_.getInt32(uint32_t(3 + k)), exitBlocks.back());
    }
    if (returns) {
      lowering_.functionFrame().switchTo(returnBlock);
      emitReturnOf(value);
      lowering_.builder_.CreateUnreachable();
    }
    for (size_t k = 0; k < exits.size(); ++k) {
      lowering_.functionFrame().switchTo(exitBlocks[k]);
      emitJumpTo(exits[k].target, exits[k].isBreak);
      lowering_.builder_.CreateUnreachable();
    }
  }
  lowering_.tries_.pop_back();
  lowering_.functionFrame().switchTo(endBlock);
}

}
