#include "compiler/lower/stmt/exit_lowerer/exit_lowerer.h"
#include "compiler/lower/async/async_lowerer/async_lowerer.h"
#include "compiler/lower/function_frame/function_frame.h"
#include "compiler/lower/iterators/coroutine_lowerer/coroutine_lowerer.h"
#include "compiler/lower/types/optional_lowerer/optional_lowerer.h"

namespace tz {

ExitLowerer::ExitLowerer(Lowering &owner)
    : lowering(owner) {}

const Type *ExitLowerer::returnedType(const FunctionDecl *fn, const Type *returnType) {
  if (fn && fn->isGenerator && fn->sym && fn->sym->hasReturnType() &&
      fn->sym->getKnownReturnType()->isIterator()) {
    return fn->sym->getKnownReturnType()->returned();
  }
  if (fn && fn->isAsync && returnType && returnType->isPromise()) {
    return returnType->promised();
  }

  return returnType;
}

void ExitLowerer::emitJumpTo(size_t target, bool isBreak) {
  const Lowering::JumpTarget &jt = this->lowering.targets[target];
  bool inGenerator = !isBreak && jt.continueDepth != ~size_t(0);
  this->lowering.functionFrame().emitCleanups(inGenerator ? jt.continueDepth : jt.scopeDepth);
  this->lowering.builder.CreateBr(isBreak ? jt.breakTarget : jt.continueTarget);
  this->lowering.functionFrame().startDeadBlock();
}

void ExitLowerer::emitReturnOf(llvm::Value *value) {
  if (this->lowering.coro && this->lowering.currentFn && this->lowering.currentFn->isGenerator) {
    const Type *type =
        ExitLowerer::returnedType(this->lowering.currentFn, this->lowering.returnType);
    this->lowering.coroutineLowerer().emitCoroutineReturn(value, value ? type : nullptr);
    return;
  }
  if (this->lowering.async) {
    this->lowering.asyncLowerer().emitAsyncReturn(value);
    return;
  }
  this->lowering.functionFrame().emitCleanups(0);
  if (value) {
    this->lowering.builder.CreateRet(value);
  } else if (this->lowering.returnType && this->lowering.returnType->isOptional()) {
    this->lowering.builder.CreateRet(
        this->lowering.optionalLowerer().undefinedValue(this->lowering.returnType));
  } else {
    this->lowering.builder.CreateRetVoid();
  }
  this->lowering.functionFrame().startDeadBlock();
}

}
