#include "compiler/lower/iterators/coroutine_lowerer/coroutine_lowerer.h"
#include "compiler/check/checker/checker.h"
#include "compiler/lower/errors/throw_lowerer/throw_lowerer.h"
#include "compiler/lower/errors/try_lowerer/try_lowerer.h"
#include "compiler/lower/expr/expr_lowerer/expr_lowerer.h"
#include "compiler/lower/function_frame/function_frame.h"
#include "compiler/lower/iterators/async_generator_lowerer/async_generator_lowerer.h"
#include "compiler/lower/iterators/generator_lowerer/generator_lowerer.h"
#include "compiler/lower/memory/ref_counts/ref_counts.h"
#include "compiler/lower/module_builder/module_builder.h"
#include "compiler/lower/types/optional_lowerer/optional_lowerer.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/IR/MDBuilder.h"
#include "llvm/Support/ErrorHandling.h"

using llvm::BasicBlock;
using llvm::cast;
using llvm::dyn_cast;
using llvm::isa;
using llvm::Value;

namespace tz {

namespace {

// The header before the frame of the coroutine: { i64 rc, ptr layout }.
constexpr uint64_t kHeaderSize = 16;

// The alignment of the frame, which follows the header of an allocation.
constexpr uint32_t kFrameAlign = 8;

enum PromiseField : unsigned { kOut, kIn, kRet, kRunning, kClose };

} // namespace

CoroutineLowerer::CoroutineLowerer(Lowering &lowering) : lowering_(lowering) {}

llvm::Function *CoroutineLowerer::coroIntrinsic(llvm::Intrinsic::ID id,
                                                llvm::ArrayRef<llvm::Type *> types) {
  return llvm::Intrinsic::getOrInsertDeclaration(lowering_.module_.get(), id, types);
}

Lowering::EmitContext CoroutineLowerer::saveContext() {
  return {lowering_.currentFn_, lowering_.envParam_, lowering_.returnType_,
          lowering_.tailGroup_, lowering_.targets_,  lowering_.inline_};
}

void CoroutineLowerer::restoreContext(const Lowering::EmitContext &context) {
  lowering_.currentFn_ = context.fn;
  lowering_.envParam_ = context.env;
  lowering_.returnType_ = context.returnType;
  lowering_.tailGroup_ = context.tailGroup;
  lowering_.targets_ = context.targets;
  lowering_.inline_ = context.inlined;
}

// The start of a coroutine: its frame is allocated after a header that makes
// it an object of the runtime, which the ramp returns.
void CoroutineLowerer::beginCoroutine(unsigned layoutFlags, const Type *sent,
                                      llvm::StructType *promise) {
  lowering_.function_->addFnAttr(llvm::Attribute::PresplitCoroutine);
  Lowering::Coroutine &c = *lowering_.coro_;
  llvm::Type *i8 = lowering_.builder_.getInt8Ty();
  llvm::Value *null = llvm::ConstantPointerNull::get(lowering_.builder_.getPtrTy());
  c.sent = sent;
  c.promise = lowering_.functionFrame().createStackSlot(promise ? promise : lowering_.promiseTy_,
                                                        "promise");
  c.id = lowering_.builder_.CreateCall(
      coroIntrinsic(llvm::Intrinsic::coro_id),
      {lowering_.builder_.getInt32(kFrameAlign), c.promise, null, null}, "id");
  llvm::Value *size = lowering_.builder_.CreateCall(
      coroIntrinsic(llvm::Intrinsic::coro_size, {lowering_.builder_.getInt64Ty()}), {}, "size");
  llvm::Value *memory = lowering_.builder_.CreateCall(
      lowering_.rt_.objAlloc,
      {lowering_.builder_.CreateAdd(size, lowering_.builder_.getInt64(kHeaderSize))}, "memory");
  lowering_.builder_.CreateStore(lowering_.builder_.getInt64(1), memory);
  lowering_.builder_.CreateStore(lowering_.generatorLowerer().iteratorLayout(layoutFlags),
                                 lowering_.builder_.CreateConstInBoundsGEP1_64(i8, memory, 8));
  llvm::Value *frame =
      lowering_.builder_.CreateConstInBoundsGEP1_64(i8, memory, kHeaderSize, "frame");
  c.handle = lowering_.builder_.CreateCall(coroIntrinsic(llvm::Intrinsic::coro_begin),
                                           {c.id, frame}, "handle");
  // The object the ramp returns comes from the handle: LLVM takes the frame
  // to be reached only through it.
  c.object = lowering_.builder_.CreateGEP(
      i8, c.handle,
      llvm::ConstantInt::getSigned(lowering_.builder_.getInt64Ty(), -int64_t(kHeaderSize)),
      "iterator");
  lowering_.builder_.CreateStore(
      llvm::Constant::getNullValue(promise ? promise : lowering_.promiseTy_), c.promise);
  c.finalBlock = lowering_.functionFrame().newBlock("coro.final");
  c.freeBlock = lowering_.functionFrame().newBlock("coro.free");
  c.returnBlock = lowering_.functionFrame().newBlock("coro.return");
}

void CoroutineLowerer::emitSuspend() {
  Lowering::Coroutine &c = *lowering_.coro_;
  llvm::Value *state = lowering_.builder_.CreateCall(
      coroIntrinsic(llvm::Intrinsic::coro_suspend),
      {llvm::ConstantTokenNone::get(lowering_.ctx_), lowering_.builder_.getFalse()}, "suspend");
  llvm::BasicBlock *resumeBlock = lowering_.functionFrame().newBlock("coro.resume");
  llvm::BasicBlock *destroyBlock = lowering_.functionFrame().newBlock("coro.destroy");
  llvm::SwitchInst *next = lowering_.builder_.CreateSwitch(state, c.returnBlock, 2);
  next->addCase(lowering_.builder_.getInt8(0), resumeBlock);
  next->addCase(lowering_.builder_.getInt8(1), destroyBlock);
  // Destroyed while it waits here: what its scopes hold is released.
  lowering_.functionFrame().switchTo(destroyBlock);
  bool noAwait = std::exchange(lowering_.cleanupNoAwait_, true);
  lowering_.functionFrame().emitCleanups(0);
  lowering_.cleanupNoAwait_ = noAwait;
  lowering_.builder_.CreateBr(c.freeBlock);
  lowering_.functionFrame().switchTo(resumeBlock);
}

llvm::Value *CoroutineLowerer::emitCoroutineYield(llvm::Value *value, const Type *resultType) {
  Lowering::Coroutine &c = *lowering_.coro_;
  if (!c.closes) {
    llvm::Value *out = lowering_.builder_.CreateLoad(
        lowering_.builder_.getPtrTy(),
        lowering_.builder_.CreateStructGEP(lowering_.promiseTy_, c.promise, kOut), "out");
    lowering_.builder_.CreateStore(value, out);
    emitSuspend();
  } else {
    // Whether a `finally` waits here, which closing the generator runs; and
    // while it closes, a `yield` in a `finally` gives nothing: the generator
    // stops there, as no one asks for more.
    bool closable = lowering_.tryLowerer().finallyBetween(~size_t(0)) != nullptr;
    llvm::Value *close =
        lowering_.builder_.CreateStructGEP(lowering_.promiseTy_, c.promise, kClose);
    llvm::BasicBlock *giveBlock = lowering_.functionFrame().newBlock("yield.give");
    llvm::BasicBlock *dropBlock = lowering_.functionFrame().newBlock("yield.closing");
    llvm::BasicBlock *suspendBlock = lowering_.functionFrame().newBlock("yield.suspend");
    lowering_.builder_.CreateCondBr(
        lowering_.builder_.CreateICmpEQ(
            lowering_.builder_.CreateLoad(lowering_.builder_.getInt32Ty(), close),
            lowering_.builder_.getInt32(2)),
        dropBlock, giveBlock);
    lowering_.functionFrame().switchTo(giveBlock);
    llvm::Value *out = lowering_.builder_.CreateLoad(
        lowering_.builder_.getPtrTy(),
        lowering_.builder_.CreateStructGEP(lowering_.promiseTy_, c.promise, kOut), "out");
    lowering_.builder_.CreateStore(value, out);
    lowering_.builder_.CreateStore(lowering_.builder_.getInt32(closable ? 1 : 0), close);
    lowering_.builder_.CreateBr(suspendBlock);
    lowering_.functionFrame().switchTo(dropBlock);
    lowering_.refCounts().releaseValue(value, c.yielded);
    lowering_.builder_.CreateBr(suspendBlock);
    lowering_.functionFrame().switchTo(suspendBlock);
    emitSuspend();
    if (closable) {
      // Resumed to close: a `return` from here.
      llvm::BasicBlock *closeBlock = lowering_.functionFrame().newBlock("yield.close");
      llvm::BasicBlock *goBlock = lowering_.functionFrame().newBlock("yield.resumed");
      lowering_.builder_.CreateCondBr(
          lowering_.builder_.CreateICmpEQ(
              lowering_.builder_.CreateLoad(lowering_.builder_.getInt32Ty(), close),
              lowering_.builder_.getInt32(2)),
          closeBlock, goBlock, llvm::MDBuilder(lowering_.ctx_).createUnlikelyBranchWeights());
      lowering_.functionFrame().switchTo(closeBlock);
      lowering_.tryLowerer().emitReturnOf(nullptr);
      if (lowering_.functionFrame().blockIsOpen()) {
        lowering_.builder_.CreateUnreachable();
      }
      lowering_.functionFrame().switchTo(goBlock);
    }
  }
  if (!resultType) {
    return nullptr;
  }
  if (resultType->isUndefined()) {
    return lowering_.builder_.getFalse();
  }
  if (!c.sent) {
    return lowering_.optionalLowerer().undefinedValue(resultType);
  }
  // What next(x) sent, which the caller keeps: a copy for the generator.
  llvm::Value *in = lowering_.builder_.CreateLoad(
      lowering_.builder_.getPtrTy(),
      lowering_.builder_.CreateStructGEP(lowering_.promiseTy_, c.promise, kIn), "in");
  llvm::BasicBlock *before = lowering_.builder_.GetInsertBlock();
  llvm::BasicBlock *givenBlock = lowering_.functionFrame().newBlock("yield.sent");
  llvm::BasicBlock *endBlock = lowering_.functionFrame().newBlock("yield.end");
  llvm::BasicBlock *noneBlock = endBlock;
  if (!resultType->isOptional()) {
    noneBlock = lowering_.functionFrame().newBlock("yield.none");
  }
  lowering_.builder_.CreateCondBr(lowering_.builder_.CreateIsNotNull(in), givenBlock, noneBlock);
  if (!resultType->isOptional()) {
    // next() without a value resumed a `yield` whose value is needed.
    lowering_.functionFrame().switchTo(noneBlock);
    lowering_.throwLowerer().emitThrowNew(1,
                                          "next() resumed a 'yield' that needs a value of type '" +
                                              resultType->name().str() + "' without one");
  }
  lowering_.functionFrame().switchTo(givenBlock);
  llvm::Value *sent =
      lowering_.builder_.CreateLoad(lowering_.moduleBuilder().lower(c.sent), in, "sent");
  lowering_.refCounts().retainValue(sent, c.sent);
  sent = lowering_.exprLowerer().emitConversion(sent, c.sent, resultType);
  llvm::BasicBlock *givenEnd = lowering_.builder_.GetInsertBlock();
  lowering_.builder_.CreateBr(endBlock);
  lowering_.functionFrame().switchTo(endBlock);
  llvm::PHINode *result =
      lowering_.builder_.CreatePHI(lowering_.moduleBuilder().lower(resultType), 2);
  result->addIncoming(sent, givenEnd);
  if (resultType->isOptional()) {
    result->addIncoming(lowering_.optionalLowerer().undefinedValue(resultType), before);
  }

  return result;
}

// The coroutine is done: what it returns goes where the caller wants it, if
// anywhere, and it waits at its final suspension to be destroyed.
void CoroutineLowerer::emitCoroutineReturn(llvm::Value *value, const Type *type) {
  Lowering::Coroutine &c = *lowering_.coro_;
  if (value && type && !type->isVoid() && !type->isUndefined()) {
    llvm::Value *ret = lowering_.builder_.CreateLoad(
        lowering_.builder_.getPtrTy(),
        lowering_.builder_.CreateStructGEP(lowering_.promiseTy_, c.promise, kRet), "ret");
    llvm::BasicBlock *storeBlock = lowering_.functionFrame().newBlock("return.store");
    llvm::BasicBlock *dropBlock = lowering_.functionFrame().newBlock("return.drop");
    llvm::BasicBlock *doneBlock = lowering_.functionFrame().newBlock("return.done");
    lowering_.builder_.CreateCondBr(lowering_.builder_.CreateIsNotNull(ret), storeBlock, dropBlock);
    lowering_.functionFrame().switchTo(storeBlock);
    lowering_.builder_.CreateStore(value, ret);
    lowering_.builder_.CreateBr(doneBlock);
    lowering_.functionFrame().switchTo(dropBlock);
    lowering_.refCounts().releaseValue(value, type);
    lowering_.builder_.CreateBr(doneBlock);
    lowering_.functionFrame().switchTo(doneBlock);
  }
  lowering_.functionFrame().emitCleanups(0);
  if (lowering_.async_ && lowering_.async_->generator) {
    lowering_.asyncGeneratorLowerer().finishAsyncGenerator(nullptr);
  }
  lowering_.builder_.CreateBr(c.finalBlock);
  lowering_.functionFrame().startDeadBlock();
}

void CoroutineLowerer::finishCoroutine() {
  Lowering::Coroutine &c = *lowering_.coro_;
  if (lowering_.functionFrame().blockIsOpen()) {
    lowering_.builder_.CreateUnreachable();
  }
  llvm::Value *none = llvm::ConstantTokenNone::get(lowering_.ctx_);
  // The final suspension; next() never resumes it from there.
  lowering_.functionFrame().switchTo(c.finalBlock);
  llvm::Value *state = lowering_.builder_.CreateCall(coroIntrinsic(llvm::Intrinsic::coro_suspend),
                                                     {none, lowering_.builder_.getTrue()}, "final");
  llvm::BasicBlock *resumedBlock = lowering_.functionFrame().newBlock("coro.resumed.done");
  llvm::SwitchInst *next = lowering_.builder_.CreateSwitch(state, c.returnBlock, 2);
  next->addCase(lowering_.builder_.getInt8(0), resumedBlock);
  next->addCase(lowering_.builder_.getInt8(1), c.freeBlock);
  lowering_.functionFrame().switchTo(resumedBlock);
  lowering_.builder_.CreateUnreachable();

  // Freeing: the frame starts after the header of the object.
  lowering_.functionFrame().switchTo(c.freeBlock);
  llvm::Value *frame = lowering_.builder_.CreateCall(coroIntrinsic(llvm::Intrinsic::coro_free),
                                                     {c.id, c.handle}, "mem");
  llvm::BasicBlock *freeObject = lowering_.functionFrame().newBlock("coro.free.object");
  lowering_.builder_.CreateCondBr(lowering_.builder_.CreateIsNotNull(frame), freeObject,
                                  c.returnBlock);
  lowering_.functionFrame().switchTo(freeObject);
  llvm::Value *back =
      llvm::ConstantInt::getSigned(lowering_.builder_.getInt64Ty(), -int64_t(c.header));
  lowering_.builder_.CreateCall(
      lowering_.rt_.objFree,
      {lowering_.builder_.CreateGEP(lowering_.builder_.getInt8Ty(), frame, back)});
  lowering_.builder_.CreateBr(c.returnBlock);

  lowering_.functionFrame().switchTo(c.returnBlock);
  lowering_.builder_.CreateCall(coroIntrinsic(llvm::Intrinsic::coro_end),
                                {c.handle, lowering_.builder_.getFalse(), none});
  lowering_.builder_.CreateRet(c.object);
}

llvm::Value *CoroutineLowerer::emitResume(llvm::Value *it, llvm::Value *out, llvm::Value *in,
                                          llvm::Value *ret) {
  llvm::Type *ptr = lowering_.builder_.getPtrTy();
  llvm::Value *frame = lowering_.builder_.CreateConstInBoundsGEP1_64(lowering_.builder_.getInt8Ty(),
                                                                     it, kHeaderSize, "frame");
  llvm::BasicBlock *before = lowering_.builder_.GetInsertBlock();
  llvm::BasicBlock *resumeBlock = lowering_.functionFrame().newBlock("next.resume");
  llvm::BasicBlock *endBlock = lowering_.functionFrame().newBlock("next.end");
  llvm::Value *done =
      lowering_.builder_.CreateCall(coroIntrinsic(llvm::Intrinsic::coro_done), {frame}, "done");
  lowering_.builder_.CreateCondBr(done, endBlock, resumeBlock);

  lowering_.functionFrame().switchTo(resumeBlock);
  llvm::Value *promise = lowering_.builder_.CreateCall(
      coroIntrinsic(llvm::Intrinsic::coro_promise),
      {frame, lowering_.builder_.getInt32(kFrameAlign), lowering_.builder_.getFalse()}, "promise");
  // A generator that calls next() on itself, as JavaScript reports.
  llvm::Value *running =
      lowering_.builder_.CreateStructGEP(lowering_.promiseTy_, promise, kRunning);
  llvm::BasicBlock *runningBlock = lowering_.functionFrame().newBlock("next.running");
  llvm::BasicBlock *goBlock = lowering_.functionFrame().newBlock("next.go");
  lowering_.builder_.CreateCondBr(
      lowering_.builder_.CreateICmpNE(
          lowering_.builder_.CreateLoad(lowering_.builder_.getInt32Ty(), running),
          lowering_.builder_.getInt32(0)),
      runningBlock, goBlock, llvm::MDBuilder(lowering_.ctx_).createUnlikelyBranchWeights());
  lowering_.functionFrame().switchTo(runningBlock);
  lowering_.throwLowerer().emitThrowNew(1, "Generator is already running");
  lowering_.functionFrame().switchTo(goBlock);
  llvm::Value *null = llvm::ConstantPointerNull::get(ptr);
  lowering_.builder_.CreateStore(
      out, lowering_.builder_.CreateStructGEP(lowering_.promiseTy_, promise, kOut));
  lowering_.builder_.CreateStore(
      in ? in : null, lowering_.builder_.CreateStructGEP(lowering_.promiseTy_, promise, kIn));
  lowering_.builder_.CreateStore(
      ret ? ret : null, lowering_.builder_.CreateStructGEP(lowering_.promiseTy_, promise, kRet));
  lowering_.builder_.CreateStore(lowering_.builder_.getInt32(1), running);
  lowering_.builder_.CreateCall(coroIntrinsic(llvm::Intrinsic::coro_resume), {frame});
  lowering_.builder_.CreateStore(lowering_.builder_.getInt32(0), running);
  // Its code threw: the generator is done, and the error goes on here.
  lowering_.throwLowerer().emitThrowCheck();
  llvm::Value *gave = lowering_.builder_.CreateNot(
      lowering_.builder_.CreateCall(coroIntrinsic(llvm::Intrinsic::coro_done), {frame}));
  llvm::BasicBlock *resumeEnd = lowering_.builder_.GetInsertBlock();
  lowering_.builder_.CreateBr(endBlock);

  lowering_.functionFrame().switchTo(endBlock);
  llvm::PHINode *has = lowering_.builder_.CreatePHI(lowering_.builder_.getInt1Ty(), 2, "has");
  has->addIncoming(lowering_.builder_.getFalse(), before);
  has->addIncoming(gave, resumeEnd);

  return has;
}

} // namespace tz
