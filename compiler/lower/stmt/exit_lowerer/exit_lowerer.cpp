#include "compiler/lower/stmt/exit_lowerer/exit_lowerer.h"
#include "compiler/lower/async/async_lowerer/async_lowerer.h"
#include "compiler/lower/function_frame/function_frame.h"
#include "compiler/lower/iterators/coroutine_lowerer/coroutine_lowerer.h"
#include "compiler/lower/types/optional_lowerer/optional_lowerer.h"

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

ExitLowerer::ExitLowerer(Lowering &owner)
    : lowering(owner) {}

void ExitLowerer::emitJumpTo(size_t target, bool isBreak) {
  const Lowering::JumpTarget &jt = this->lowering.targets_[target];
  bool inGenerator = !isBreak && jt.continueDepth != ~size_t(0);
  this->lowering.functionFrame().emitCleanups(inGenerator ? jt.continueDepth : jt.scopeDepth);
  this->lowering.builder_.CreateBr(isBreak ? jt.breakTarget : jt.continueTarget);
  this->lowering.functionFrame().startDeadBlock();
}

void ExitLowerer::emitReturnOf(llvm::Value *value) {
  if (this->lowering.coro_ && this->lowering.currentFn_ && this->lowering.currentFn_->isGenerator) {
    const Type *type = returnedType(this->lowering.currentFn_, this->lowering.returnType_);
    this->lowering.coroutineLowerer().emitCoroutineReturn(value, value ? type : nullptr);
    return;
  }
  if (this->lowering.async_) {
    this->lowering.asyncLowerer().emitAsyncReturn(value);
    return;
  }
  this->lowering.functionFrame().emitCleanups(0);
  if (value) {
    this->lowering.builder_.CreateRet(value);
  } else if (this->lowering.returnType_ && this->lowering.returnType_->isOptional()) {
    this->lowering.builder_.CreateRet(
        this->lowering.optionalLowerer().undefinedValue(this->lowering.returnType_));
  } else {
    this->lowering.builder_.CreateRetVoid();
  }
  this->lowering.functionFrame().startDeadBlock();
}

}
