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

ExitLowerer::ExitLowerer(Lowering &lowering) : lowering_(lowering) {}

void ExitLowerer::emitJumpTo(size_t target, bool isBreak) {
  const Lowering::JumpTarget &jt = lowering_.targets_[target];
  bool inGenerator = !isBreak && jt.continueDepth != ~size_t(0);
  lowering_.functionFrame().emitCleanups(inGenerator ? jt.continueDepth : jt.scopeDepth);
  lowering_.builder_.CreateBr(isBreak ? jt.breakTarget : jt.continueTarget);
  lowering_.functionFrame().startDeadBlock();
}

void ExitLowerer::emitReturnOf(llvm::Value *value) {
  if (lowering_.coro_ && lowering_.currentFn_ && lowering_.currentFn_->isGenerator) {
    const Type *type = returnedType(lowering_.currentFn_, lowering_.returnType_);
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

}
