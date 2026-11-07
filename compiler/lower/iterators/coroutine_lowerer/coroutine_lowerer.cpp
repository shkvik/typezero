#include "compiler/lower/iterators/coroutine_lowerer/coroutine_lowerer.h"
#include "compiler/check/checker/checker.h"
#include "compiler/library/process/process_lowerer/process_lowerer.h"
#include "compiler/lower/expr/expr_lowerer/expr_lowerer.h"
#include "compiler/lower/function_frame/function_frame.h"
#include "compiler/lower/iterators/async_generator_lowerer/async_generator_lowerer.h"
#include "compiler/lower/iterators/generator_lowerer/generator_lowerer.h"
#include "compiler/lower/memory/ref_counts/ref_counts.h"
#include "compiler/lower/module_builder/module_builder.h"
#include "compiler/lower/stmt/exit_lowerer/exit_lowerer.h"
#include "compiler/lower/types/optional_lowerer/optional_lowerer.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/IR/MDBuilder.h"


namespace tz {

namespace {

constexpr uint64_t kHeaderSize = 16;

constexpr uint32_t kFrameAlign = 8;

enum PromiseField : unsigned { kOut, kIn, kRet, kRunning };

}

CoroutineLowerer::CoroutineLowerer(Lowering &owner)
    : lowering(owner) {}

llvm::Function *CoroutineLowerer::coroIntrinsic(llvm::Intrinsic::ID id,
                                                llvm::ArrayRef<llvm::Type *> types) {
  return llvm::Intrinsic::getOrInsertDeclaration(this->lowering.module_.get(), id, types);
}

Lowering::EmitContext CoroutineLowerer::saveContext() {
  return {this->lowering.currentFn_, this->lowering.envParam_, this->lowering.returnType_,
          this->lowering.tailGroup_, this->lowering.targets_,  this->lowering.inline_};
}

void CoroutineLowerer::restoreContext(const Lowering::EmitContext &context) {
  this->lowering.currentFn_ = context.fn;
  this->lowering.envParam_ = context.env;
  this->lowering.returnType_ = context.returnType;
  this->lowering.tailGroup_ = context.tailGroup;
  this->lowering.targets_ = context.targets;
  this->lowering.inline_ = context.inlined;
}

void CoroutineLowerer::beginCoroutine(unsigned layoutFlags, const Type *sent,
                                      llvm::StructType *promise) {
  this->lowering.function_->addFnAttr(llvm::Attribute::PresplitCoroutine);
  Lowering::Coroutine &c = *this->lowering.coro_;
  llvm::Type *i8 = this->lowering.builder_.getInt8Ty();
  llvm::Value *null = llvm::ConstantPointerNull::get(this->lowering.builder_.getPtrTy());
  c.sent = sent;
  c.promise = this->lowering.functionFrame().createStackSlot(
      promise ? promise : this->lowering.promiseTy_, "promise");
  c.id = this->lowering.builder_.CreateCall(
      this->coroIntrinsic(llvm::Intrinsic::coro_id),
      {this->lowering.builder_.getInt32(kFrameAlign), c.promise, null, null}, "id");
  llvm::Value *size = this->lowering.builder_.CreateCall(
      this->coroIntrinsic(llvm::Intrinsic::coro_size, {this->lowering.builder_.getInt64Ty()}), {},
      "size");
  llvm::Value *memory = this->lowering.builder_.CreateCall(
      this->lowering.rt_.objAlloc,
      {this->lowering.builder_.CreateAdd(size, this->lowering.builder_.getInt64(kHeaderSize))},
      "memory");
  this->lowering.builder_.CreateStore(this->lowering.builder_.getInt64(1), memory);
  this->lowering.builder_.CreateStore(
      this->lowering.generatorLowerer().iteratorLayout(layoutFlags),
      this->lowering.builder_.CreateConstInBoundsGEP1_64(i8, memory, 8));
  llvm::Value *frame =
      this->lowering.builder_.CreateConstInBoundsGEP1_64(i8, memory, kHeaderSize, "frame");
  c.handle = this->lowering.builder_.CreateCall(this->coroIntrinsic(llvm::Intrinsic::coro_begin),
                                                {c.id, frame}, "handle");
  c.object = this->lowering.builder_.CreateGEP(
      i8, c.handle,
      llvm::ConstantInt::getSigned(this->lowering.builder_.getInt64Ty(), -int64_t(kHeaderSize)),
      "iterator");
  this->lowering.builder_.CreateStore(
      llvm::Constant::getNullValue(promise ? promise : this->lowering.promiseTy_), c.promise);
  c.finalBlock = this->lowering.functionFrame().newBlock("coro.final");
  c.freeBlock = this->lowering.functionFrame().newBlock("coro.free");
  c.returnBlock = this->lowering.functionFrame().newBlock("coro.return");
}

llvm::Value *CoroutineLowerer::emitSave() {
  return this->lowering.builder_.CreateCall(this->coroIntrinsic(llvm::Intrinsic::coro_save),
                                            {this->lowering.coro_->handle}, "save");
}

void CoroutineLowerer::emitSuspend(llvm::Value *save) {
  Lowering::Coroutine &c = *this->lowering.coro_;
  if (!save) {
    save = llvm::ConstantTokenNone::get(this->lowering.ctx_);
  }
  llvm::Value *state =
      this->lowering.builder_.CreateCall(this->coroIntrinsic(llvm::Intrinsic::coro_suspend),
                                         {save, this->lowering.builder_.getFalse()}, "suspend");
  llvm::BasicBlock *resumeBlock = this->lowering.functionFrame().newBlock("coro.resume");
  llvm::BasicBlock *destroyBlock = this->lowering.functionFrame().newBlock("coro.destroy");
  llvm::SwitchInst *next = this->lowering.builder_.CreateSwitch(state, c.returnBlock, 2);
  next->addCase(this->lowering.builder_.getInt8(0), resumeBlock);
  next->addCase(this->lowering.builder_.getInt8(1), destroyBlock);
  this->lowering.functionFrame().switchTo(destroyBlock);
  bool noAwait = std::exchange(this->lowering.cleanupNoAwait_, true);
  this->lowering.functionFrame().emitCleanups(0);
  this->lowering.cleanupNoAwait_ = noAwait;
  this->lowering.builder_.CreateBr(c.freeBlock);
  this->lowering.functionFrame().switchTo(resumeBlock);
}

llvm::Value *CoroutineLowerer::emitCoroutineYield(llvm::Value *value, const Type *resultType) {
  Lowering::Coroutine &c = *this->lowering.coro_;
  llvm::Value *out = this->lowering.builder_.CreateLoad(
      this->lowering.builder_.getPtrTy(),
      this->lowering.builder_.CreateStructGEP(this->lowering.promiseTy_, c.promise, kOut), "out");
  this->lowering.builder_.CreateStore(value, out);
  this->emitSuspend();
  if (!resultType) {
    return nullptr;
  }
  if (resultType->isUndefined()) {
    return this->lowering.builder_.getFalse();
  }
  if (!c.sent) {
    return this->lowering.optionalLowerer().undefinedValue(resultType);
  }
  llvm::Value *in = this->lowering.builder_.CreateLoad(
      this->lowering.builder_.getPtrTy(),
      this->lowering.builder_.CreateStructGEP(this->lowering.promiseTy_, c.promise, kIn), "in");
  llvm::BasicBlock *before = this->lowering.builder_.GetInsertBlock();
  llvm::BasicBlock *givenBlock = this->lowering.functionFrame().newBlock("yield.sent");
  llvm::BasicBlock *endBlock = this->lowering.functionFrame().newBlock("yield.end");
  llvm::BasicBlock *noneBlock = endBlock;
  if (!resultType->isOptional()) {
    noneBlock = this->lowering.functionFrame().newBlock("yield.none");
  }
  this->lowering.builder_.CreateCondBr(this->lowering.builder_.CreateIsNotNull(in), givenBlock,
                                       noneBlock);
  if (!resultType->isOptional()) {
    this->lowering.functionFrame().switchTo(noneBlock);
    this->lowering.processLowerer().emitPanic(
        "next() resumed a 'yield' that needs a value of type '" + resultType->name().str() +
        "' without one");
  }
  this->lowering.functionFrame().switchTo(givenBlock);
  llvm::Value *sent =
      this->lowering.builder_.CreateLoad(this->lowering.moduleBuilder().lower(c.sent), in, "sent");
  this->lowering.refCounts().retainValue(sent, c.sent);
  sent = this->lowering.exprLowerer().emitConversion(sent, c.sent, resultType);
  llvm::BasicBlock *givenEnd = this->lowering.builder_.GetInsertBlock();
  this->lowering.builder_.CreateBr(endBlock);
  this->lowering.functionFrame().switchTo(endBlock);
  llvm::PHINode *result =
      this->lowering.builder_.CreatePHI(this->lowering.moduleBuilder().lower(resultType), 2);
  result->addIncoming(sent, givenEnd);
  if (resultType->isOptional()) {
    result->addIncoming(this->lowering.optionalLowerer().undefinedValue(resultType), before);
  }

  return result;
}

void CoroutineLowerer::emitCoroutineReturn(llvm::Value *value, const Type *type) {
  Lowering::Coroutine &c = *this->lowering.coro_;
  if (value && type && !type->isVoid() && !type->isUndefined()) {
    llvm::Value *ret = this->lowering.builder_.CreateLoad(
        this->lowering.builder_.getPtrTy(),
        this->lowering.builder_.CreateStructGEP(this->lowering.promiseTy_, c.promise, kRet), "ret");
    llvm::BasicBlock *storeBlock = this->lowering.functionFrame().newBlock("return.store");
    llvm::BasicBlock *dropBlock = this->lowering.functionFrame().newBlock("return.drop");
    llvm::BasicBlock *doneBlock = this->lowering.functionFrame().newBlock("return.done");
    this->lowering.builder_.CreateCondBr(this->lowering.builder_.CreateIsNotNull(ret), storeBlock,
                                         dropBlock);
    this->lowering.functionFrame().switchTo(storeBlock);
    this->lowering.builder_.CreateStore(value, ret);
    this->lowering.builder_.CreateBr(doneBlock);
    this->lowering.functionFrame().switchTo(dropBlock);
    this->lowering.refCounts().releaseValue(value, type);
    this->lowering.builder_.CreateBr(doneBlock);
    this->lowering.functionFrame().switchTo(doneBlock);
  }
  this->lowering.functionFrame().emitCleanups(0);
  if (this->lowering.async_ && this->lowering.async_->generator) {
    this->lowering.asyncGeneratorLowerer().finishAsyncGenerator();
  }
  this->lowering.builder_.CreateBr(c.finalBlock);
  this->lowering.functionFrame().startDeadBlock();
}

void CoroutineLowerer::finishCoroutine() {
  Lowering::Coroutine &c = *this->lowering.coro_;
  if (this->lowering.functionFrame().blockIsOpen()) {
    this->lowering.builder_.CreateUnreachable();
  }
  llvm::Value *none = llvm::ConstantTokenNone::get(this->lowering.ctx_);
  this->lowering.functionFrame().switchTo(c.finalBlock);
  llvm::Value *state =
      this->lowering.builder_.CreateCall(this->coroIntrinsic(llvm::Intrinsic::coro_suspend),
                                         {none, this->lowering.builder_.getTrue()}, "final");
  llvm::BasicBlock *resumedBlock = this->lowering.functionFrame().newBlock("coro.resumed.done");
  llvm::SwitchInst *next = this->lowering.builder_.CreateSwitch(state, c.returnBlock, 2);
  next->addCase(this->lowering.builder_.getInt8(0), resumedBlock);
  next->addCase(this->lowering.builder_.getInt8(1), c.freeBlock);
  this->lowering.functionFrame().switchTo(resumedBlock);
  this->lowering.builder_.CreateUnreachable();

  this->lowering.functionFrame().switchTo(c.freeBlock);
  llvm::Value *frame = this->lowering.builder_.CreateCall(
      this->coroIntrinsic(llvm::Intrinsic::coro_free), {c.id, c.handle}, "mem");
  llvm::BasicBlock *freeObject = this->lowering.functionFrame().newBlock("coro.free.object");
  this->lowering.builder_.CreateCondBr(this->lowering.builder_.CreateIsNotNull(frame), freeObject,
                                       c.returnBlock);
  this->lowering.functionFrame().switchTo(freeObject);
  llvm::Value *back =
      llvm::ConstantInt::getSigned(this->lowering.builder_.getInt64Ty(), -int64_t(c.header));
  this->lowering.builder_.CreateCall(
      this->lowering.rt_.objFree,
      {this->lowering.builder_.CreateGEP(this->lowering.builder_.getInt8Ty(), frame, back)});
  this->lowering.builder_.CreateBr(c.returnBlock);

  this->lowering.functionFrame().switchTo(c.returnBlock);
  this->lowering.builder_.CreateCall(this->coroIntrinsic(llvm::Intrinsic::coro_end),
                                     {c.handle, this->lowering.builder_.getFalse(), none});
  this->lowering.builder_.CreateRet(c.object);
}

llvm::Value *CoroutineLowerer::emitResume(llvm::Value *it, llvm::Value *out, llvm::Value *in,
                                          llvm::Value *ret) {
  llvm::Type *ptr = this->lowering.builder_.getPtrTy();
  llvm::Value *frame = this->lowering.builder_.CreateConstInBoundsGEP1_64(
      this->lowering.builder_.getInt8Ty(), it, kHeaderSize, "frame");
  llvm::BasicBlock *before = this->lowering.builder_.GetInsertBlock();
  llvm::BasicBlock *resumeBlock = this->lowering.functionFrame().newBlock("next.resume");
  llvm::BasicBlock *endBlock = this->lowering.functionFrame().newBlock("next.end");
  llvm::Value *done = this->lowering.builder_.CreateCall(
      this->coroIntrinsic(llvm::Intrinsic::coro_done), {frame}, "done");
  this->lowering.builder_.CreateCondBr(done, endBlock, resumeBlock);

  this->lowering.functionFrame().switchTo(resumeBlock);
  llvm::Value *promise = this->lowering.builder_.CreateCall(
      this->coroIntrinsic(llvm::Intrinsic::coro_promise),
      {frame, this->lowering.builder_.getInt32(kFrameAlign), this->lowering.builder_.getFalse()},
      "promise");
  llvm::Value *running =
      this->lowering.builder_.CreateStructGEP(this->lowering.promiseTy_, promise, kRunning);
  llvm::BasicBlock *runningBlock = this->lowering.functionFrame().newBlock("next.running");
  llvm::BasicBlock *goBlock = this->lowering.functionFrame().newBlock("next.go");
  this->lowering.builder_.CreateCondBr(
      this->lowering.builder_.CreateICmpNE(
          this->lowering.builder_.CreateLoad(this->lowering.builder_.getInt32Ty(), running),
          this->lowering.builder_.getInt32(0)),
      runningBlock, goBlock, llvm::MDBuilder(this->lowering.ctx_).createUnlikelyBranchWeights());
  this->lowering.functionFrame().switchTo(runningBlock);
  this->lowering.processLowerer().emitPanic("Generator is already running");
  this->lowering.functionFrame().switchTo(goBlock);
  llvm::Value *null = llvm::ConstantPointerNull::get(ptr);
  this->lowering.builder_.CreateStore(
      out, this->lowering.builder_.CreateStructGEP(this->lowering.promiseTy_, promise, kOut));
  this->lowering.builder_.CreateStore(in ? in : null, this->lowering.builder_.CreateStructGEP(
                                                          this->lowering.promiseTy_, promise, kIn));
  this->lowering.builder_.CreateStore(
      ret ? ret : null,
      this->lowering.builder_.CreateStructGEP(this->lowering.promiseTy_, promise, kRet));
  this->lowering.builder_.CreateStore(this->lowering.builder_.getInt32(1), running);
  this->lowering.builder_.CreateCall(this->coroIntrinsic(llvm::Intrinsic::coro_resume), {frame});
  this->lowering.builder_.CreateStore(this->lowering.builder_.getInt32(0), running);
  llvm::Value *gave = this->lowering.builder_.CreateNot(
      this->lowering.builder_.CreateCall(this->coroIntrinsic(llvm::Intrinsic::coro_done), {frame}));
  llvm::BasicBlock *resumeEnd = this->lowering.builder_.GetInsertBlock();
  this->lowering.builder_.CreateBr(endBlock);

  this->lowering.functionFrame().switchTo(endBlock);
  llvm::PHINode *has =
      this->lowering.builder_.CreatePHI(this->lowering.builder_.getInt1Ty(), 2, "has");
  has->addIncoming(this->lowering.builder_.getFalse(), before);
  has->addIncoming(gave, resumeEnd);

  return has;
}

}
