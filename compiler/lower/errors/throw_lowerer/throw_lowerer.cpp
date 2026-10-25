#include "compiler/lower/errors/throw_lowerer/throw_lowerer.h"
#include "compiler/library/process/process_lowerer/process_lowerer.h"
#include "compiler/lower/errors/may_throw/may_throw.h"
#include "compiler/lower/errors/unwind_lowerer/unwind_lowerer.h"
#include "compiler/lower/expr/expr_lowerer/expr_lowerer.h"
#include "compiler/lower/function_frame/function_frame.h"
#include "compiler/lower/module_builder/module_builder.h"
#include "compiler/lower/types/unknown_lowerer/unknown_lowerer.h"
#include "compiler/lower/values/string_lowerer/string_lowerer.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/IR/MDBuilder.h"
#include "llvm/Support/ConvertUTF.h"
#include "llvm/Support/FileSystem.h"
#include "llvm/Support/Path.h"
#include <functional>
#include <map>
#include <optional>

using llvm::BasicBlock;
using llvm::cast;
using llvm::dyn_cast;
using llvm::isa;
using llvm::Value;

namespace tz {

namespace {

std::u16string utf16(llvm::StringRef text) {
  llvm::SmallVector<llvm::UTF16, 32> units;
  llvm::convertUTF8ToUTF16String(text, units);

  return std::u16string(units.begin(), units.end());
}

} // namespace

ThrowLowerer::ThrowLowerer(Lowering &lowering) : lowering_(lowering) {}

// tz_thrown and the stack limit (TzThread in runtime/core/parallel/parallel.h): in a
// parallel program the thread's own, since code on the threads of the runtime
// throws at the same time. Its address then goes through
// llvm.threadlocal.address, which a coroutine does not keep across a
// suspension: it may resume on another thread. Both are in one variable, so
// that a function finds them from one address.
llvm::GlobalVariable *ThrowLowerer::thrownGlobal() {
  if (!thrown_) {
    auto *type = llvm::StructType::get(
        lowering_.ctx_, {lowering_.builder_.getPtrTy(), lowering_.builder_.getPtrTy()});
    thrown_ = cast<llvm::GlobalVariable>(lowering_.module_->getOrInsertGlobal("tz_thread", type));
    thrown_->setAlignment(llvm::Align(8));
    thrown_->setThreadLocal(lowering_.parallel_);
  }

  return thrown_;
}

llvm::Value *ThrowLowerer::thrownSlot() { return thrownSlot(lowering_.builder_); }

llvm::Value *ThrowLowerer::thrownSlot(llvm::IRBuilderBase &b) {
  return lowering_.parallel_ ? b.CreateThreadLocalAddress(thrownGlobal())
                             : static_cast<llvm::Value *>(thrownGlobal());
}

void ThrowLowerer::emitThrowCheck() {
  emitThrowCheckWith([] {});
}

void ThrowLowerer::emitThrowCheckWith(llvm::function_ref<void()> release) {
  llvm::Value *error =
      lowering_.builder_.CreateLoad(lowering_.builder_.getPtrTy(), thrownSlot(), "thrown");
  llvm::BasicBlock *throwBlock = lowering_.functionFrame().newBlock("throw");
  llvm::BasicBlock *goBlock = lowering_.functionFrame().newBlock("go");
  lowering_.builder_.CreateCondBr(lowering_.builder_.CreateIsNotNull(error), throwBlock, goBlock,
                                  llvm::MDBuilder(lowering_.ctx_).createUnlikelyBranchWeights());
  lowering_.functionFrame().switchTo(throwBlock);
  release();
  emitRuntimeThrow();
  lowering_.functionFrame().switchTo(goBlock);
}

void ThrowLowerer::emitRuntimeThrow() {
  llvm::Type *ptr = lowering_.builder_.getPtrTy();
  lowering_.builder_.CreateCall(lowering_.processLowerer().processFunction(
                                    "tz_error_frame", lowering_.builder_.getVoidTy(), {ptr}),
                                {lowering_.unwindLowerer().frameDescriptor(lowering_.loc_)});
  lowering_.builder_.CreateBr(lowering_.unwindLowerer().unwindTarget());
}

void ThrowLowerer::emitThrow(llvm::Value *error, llvm::SMLoc loc) {
  llvm::Type *ptr = lowering_.builder_.getPtrTy();
  lowering_.builder_.CreateCall(lowering_.processLowerer().processFunction(
                                    "tz_error_throw", lowering_.builder_.getVoidTy(), {ptr, ptr}),
                                {error, lowering_.unwindLowerer().frameDescriptor(loc)});
  lowering_.builder_.CreateBr(lowering_.unwindLowerer().unwindTarget());
  lowering_.functionFrame().startDeadBlock();
}

void ThrowLowerer::emitThrowNew(int kind, llvm::StringRef message) {
  llvm::Value *text = lowering_.stringLowerer().stringLiteral(utf16(message));
  llvm::Type *ptr = lowering_.builder_.getPtrTy(), *i64 = lowering_.builder_.getInt64Ty(),
             *i32 = lowering_.builder_.getInt32Ty();
  auto *fn = cast<llvm::Function>(
      lowering_.processLowerer()
          .processFunction("tz_throw_str", lowering_.builder_.getVoidTy(), {i32, ptr, i64})
          .getCallee());
  fn->addFnAttr(llvm::Attribute::Cold);
  lowering_.builder_.CreateCall(fn, {lowering_.builder_.getInt32(uint32_t(kind)),
                                     lowering_.stringLowerer().stringObj(text),
                                     lowering_.stringLowerer().stringMeta(text)});
  emitRuntimeThrow();
}

void ThrowLowerer::emitThrowStmt(ThrowStmt *stmt) {
  llvm::Value *error = lowering_.exprLowerer().emitExpr(stmt->value);
  if (stmt->value->type->isUnknown()) { // an Error, or else a TypeError
    error = lowering_.builder_.CreateCall(
        lowering_.unknownLowerer().unknownFunction(
            "tz_unknown_error", lowering_.builder_.getPtrTy(), {lowering_.builder_.getPtrTy()}),
        {error}, "error");
  }
  // `throw new Error(...)` throws from `new`, as V8 says (a NewExpr is at
  // the name of its class).
  llvm::SMLoc loc = stmt->value->loc();
  if (isa<NewExpr>(stmt->value)) {
    const char *start = stmt->loc().getPointer(), *at = loc.getPointer();
    while (at > start && (at[-1] == ' ' || at[-1] == '\t')) {
      --at;
    }
    if (at - 3 >= start && llvm::StringRef(at - 3, 3) == "new") {
      loc = llvm::SMLoc::getFromPointer(at - 3);
    }
  }
  emitThrow(error, loc);
}

void ThrowLowerer::emitStackCheck(llvm::SMLoc loc) {
  llvm::Type *ptr = lowering_.builder_.getPtrTy();
  llvm::Value *top = lowering_.builder_.CreateIntrinsic(llvm::Intrinsic::stackaddress, {ptr}, {});
  // Each thread has a stack of its own: TzThread::stack_limit.
  llvm::Value *slot = lowering_.builder_.CreateConstInBoundsGEP1_64(lowering_.builder_.getInt8Ty(),
                                                                    thrownSlot(), 8);
  llvm::Value *limit = lowering_.builder_.CreateLoad(ptr, slot, "stack.limit");
  llvm::BasicBlock *overflowBlock = lowering_.functionFrame().newBlock("stack.overflow");
  llvm::BasicBlock *goBlock = lowering_.functionFrame().newBlock("stack.ok");
  lowering_.builder_.CreateCondBr(lowering_.builder_.CreateICmpULT(top, limit), overflowBlock,
                                  goBlock,
                                  llvm::MDBuilder(lowering_.ctx_).createUnlikelyBranchWeights());
  lowering_.functionFrame().switchTo(overflowBlock);
  auto *fn = cast<llvm::Function>(
      lowering_.processLowerer()
          .processFunction("tz_throw_stack_overflow", lowering_.builder_.getVoidTy(), {})
          .getCallee());
  fn->addFnAttr(llvm::Attribute::Cold);
  lowering_.builder_.CreateCall(fn);
  llvm::SMLoc outer = std::exchange(lowering_.loc_, loc);
  emitRuntimeThrow();
  lowering_.loc_ = outer;
  lowering_.functionFrame().switchTo(goBlock);
}

Lowering::Held ThrowLowerer::hold(llvm::Value *value, const Type *type, bool needed) {
  if (!needed || !value || !type || !type->isManaged() || isa<llvm::Constant>(value)) {
    return {value, nullptr, type};
  }
  llvm::Value *slot = lowering_.functionFrame().createManagedSlot(type, "held");
  lowering_.builder_.CreateStore(value, slot);

  return {value, slot, type};
}

llvm::Value *ThrowLowerer::take(Lowering::Held &held) {
  if (!held.slot) {
    return held.value;
  }
  llvm::Type *lowered = lowering_.moduleBuilder().lower(held.type);
  held.value = lowering_.builder_.CreateLoad(lowered, held.slot);
  lowering_.builder_.CreateStore(llvm::Constant::getNullValue(lowered), held.slot);
  held.slot = nullptr;

  return held.value;
}

void ThrowLowerer::holdOperand(Lowering::Operand &operand, bool needed) {
  if (operand.owned) {
    operand.heldSlot = hold(operand.value, operand.type, needed).slot;
  }
}

void ThrowLowerer::takeOperand(Lowering::Operand &operand) {
  if (!operand.heldSlot) {
    return;
  }
  Lowering::Held held{operand.value, operand.heldSlot, operand.type};
  operand.value = take(held);
  operand.heldSlot = nullptr;
}

std::vector<llvm::Value *> ThrowLowerer::emitHeldValues(llvm::ArrayRef<Expr *> exprs) {
  std::vector<bool> laterThrows(exprs.size(), false);
  for (size_t i = exprs.size(); i > 1; --i) {
    laterThrows[i - 2] = laterThrows[i - 1] || lowering_.mayThrow().exprThrows(exprs[i - 1]);
  }
  std::vector<Lowering::Held> held;
  for (size_t i = 0; i < exprs.size(); ++i) {
    held.push_back(
        hold(lowering_.exprLowerer().emitExpr(exprs[i]), exprs[i]->type, laterThrows[i]));
  }
  std::vector<llvm::Value *> values;
  for (Lowering::Held &h : held) {
    values.push_back(take(h));
  }

  return values;
}

} // namespace tz
