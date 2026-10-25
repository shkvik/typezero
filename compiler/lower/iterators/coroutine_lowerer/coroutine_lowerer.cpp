#include "compiler/lower/iterators/coroutine_lowerer/coroutine_lowerer.h"
#include "compiler/lower/module_builder/module_builder.h"
#include "compiler/lower/function_frame/function_frame.h"
#include "compiler/lower/memory/ref_counts/ref_counts.h"
#include "compiler/lower/types/optional_lowerer/optional_lowerer.h"
#include "compiler/lower/expr/expr_lowerer/expr_lowerer.h"
#include "compiler/lower/iterators/async_generator_lowerer/async_generator_lowerer.h"
#include "compiler/lower/iterators/generator_lowerer/generator_lowerer.h"
#include "compiler/lower/errors/throw_lowerer/throw_lowerer.h"
#include "compiler/lower/errors/try_lowerer/try_lowerer.h"
#include "compiler/check/sema/sema.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/IR/MDBuilder.h"
#include "llvm/Support/ErrorHandling.h"

using llvm::cast;
using llvm::dyn_cast;
using llvm::isa;
using llvm::BasicBlock;
using llvm::Value;

namespace tz {

namespace {

// The header before the frame of the coroutine: { i64 rc, ptr layout }.
constexpr uint64_t kHeaderSize = 16;

// The alignment of the frame, which follows the header of an allocation.
constexpr uint32_t kFrameAlign = 8;

enum PromiseField : unsigned { kOut, kIn, kRet, kRunning, kClose };

} // namespace

CoroutineLowerer::CoroutineLowerer(CodeGen &codegen) : codegen_(codegen) {}

llvm::Function *CoroutineLowerer::coroIntrinsic(llvm::Intrinsic::ID id, llvm::ArrayRef<llvm::Type *> types) {
  return llvm::Intrinsic::getOrInsertDeclaration(codegen_.module_.get(), id, types);
}

CodeGen::EmitContext CoroutineLowerer::saveContext() {
  return {codegen_.currentFn_, codegen_.envParam_, codegen_.returnType_, codegen_.tailGroup_, codegen_.targets_, codegen_.inline_};
}

void CoroutineLowerer::restoreContext(const CodeGen::EmitContext &context) {
  codegen_.currentFn_ = context.fn;
  codegen_.envParam_ = context.env;
  codegen_.returnType_ = context.returnType;
  codegen_.tailGroup_ = context.tailGroup;
  codegen_.targets_ = context.targets;
  codegen_.inline_ = context.inlined;
}

// The start of a coroutine: its frame is allocated after a header that makes
// it an object of the runtime, which the ramp returns.
void CoroutineLowerer::beginCoroutine(unsigned layoutFlags, const Type *sent, llvm::StructType *promise) {
  codegen_.function_->addFnAttr(llvm::Attribute::PresplitCoroutine);
  CodeGen::Coroutine &c = *codegen_.coro_;
  llvm::Type *i8 = codegen_.builder_.getInt8Ty();
  llvm::Value *null = llvm::ConstantPointerNull::get(codegen_.builder_.getPtrTy());
  c.sent = sent;
  c.promise = codegen_.functionFrame().createStackSlot(promise ? promise : codegen_.promiseTy_, "promise");
  c.id = codegen_.builder_.CreateCall(coroIntrinsic(llvm::Intrinsic::coro_id),
                             {codegen_.builder_.getInt32(kFrameAlign), c.promise, null, null}, "id");
  llvm::Value *size = codegen_.builder_.CreateCall(
      coroIntrinsic(llvm::Intrinsic::coro_size, {codegen_.builder_.getInt64Ty()}), {}, "size");
  llvm::Value *memory = codegen_.builder_.CreateCall(
      codegen_.rt_.objAlloc, {codegen_.builder_.CreateAdd(size, codegen_.builder_.getInt64(kHeaderSize))}, "memory");
  codegen_.builder_.CreateStore(codegen_.builder_.getInt64(1), memory);
  codegen_.builder_.CreateStore(codegen_.generatorLowerer().iteratorLayout(layoutFlags),
                       codegen_.builder_.CreateConstInBoundsGEP1_64(i8, memory, 8));
  llvm::Value *frame = codegen_.builder_.CreateConstInBoundsGEP1_64(i8, memory, kHeaderSize, "frame");
  c.handle =
      codegen_.builder_.CreateCall(coroIntrinsic(llvm::Intrinsic::coro_begin), {c.id, frame}, "handle");
  // The object the ramp returns comes from the handle: LLVM takes the frame
  // to be reached only through it.
  c.object = codegen_.builder_.CreateGEP(
      i8, c.handle, llvm::ConstantInt::getSigned(codegen_.builder_.getInt64Ty(), -int64_t(kHeaderSize)),
      "iterator");
  codegen_.builder_.CreateStore(llvm::Constant::getNullValue(promise ? promise : codegen_.promiseTy_), c.promise);
  c.finalBlock = codegen_.functionFrame().newBlock("coro.final");
  c.freeBlock = codegen_.functionFrame().newBlock("coro.free");
  c.returnBlock = codegen_.functionFrame().newBlock("coro.return");
}

void CoroutineLowerer::emitSuspend() {
  CodeGen::Coroutine &c = *codegen_.coro_;
  llvm::Value *state =
      codegen_.builder_.CreateCall(coroIntrinsic(llvm::Intrinsic::coro_suspend),
                          {llvm::ConstantTokenNone::get(codegen_.ctx_), codegen_.builder_.getFalse()}, "suspend");
  llvm::BasicBlock *resumeBlock = codegen_.functionFrame().newBlock("coro.resume");
  llvm::BasicBlock *destroyBlock = codegen_.functionFrame().newBlock("coro.destroy");
  llvm::SwitchInst *next = codegen_.builder_.CreateSwitch(state, c.returnBlock, 2);
  next->addCase(codegen_.builder_.getInt8(0), resumeBlock);
  next->addCase(codegen_.builder_.getInt8(1), destroyBlock);
  // Destroyed while it waits here: what its scopes hold is released.
  codegen_.functionFrame().switchTo(destroyBlock);
  bool noAwait = std::exchange(codegen_.cleanupNoAwait_, true);
  codegen_.functionFrame().emitCleanups(0);
  codegen_.cleanupNoAwait_ = noAwait;
  codegen_.builder_.CreateBr(c.freeBlock);
  codegen_.functionFrame().switchTo(resumeBlock);
}

llvm::Value *CoroutineLowerer::emitCoroutineYield(llvm::Value *value, const Type *resultType) {
  CodeGen::Coroutine &c = *codegen_.coro_;
  if (!c.closes) {
    llvm::Value *out = codegen_.builder_.CreateLoad(
        codegen_.builder_.getPtrTy(), codegen_.builder_.CreateStructGEP(codegen_.promiseTy_, c.promise, kOut), "out");
    codegen_.builder_.CreateStore(value, out);
    emitSuspend();
  } else {
    // Whether a `finally` waits here, which closing the generator runs; and
    // while it closes, a `yield` in a `finally` gives nothing: the generator
    // stops there, as no one asks for more.
    bool closable = codegen_.tryLowerer().finallyBetween(~size_t(0)) != nullptr;
    llvm::Value *close = codegen_.builder_.CreateStructGEP(codegen_.promiseTy_, c.promise, kClose);
    llvm::BasicBlock *giveBlock = codegen_.functionFrame().newBlock("yield.give");
    llvm::BasicBlock *dropBlock = codegen_.functionFrame().newBlock("yield.closing");
    llvm::BasicBlock *suspendBlock = codegen_.functionFrame().newBlock("yield.suspend");
    codegen_.builder_.CreateCondBr(codegen_.builder_.CreateICmpEQ(codegen_.builder_.CreateLoad(codegen_.builder_.getInt32Ty(), close),
                                                codegen_.builder_.getInt32(2)),
                          dropBlock, giveBlock);
    codegen_.functionFrame().switchTo(giveBlock);
    llvm::Value *out = codegen_.builder_.CreateLoad(
        codegen_.builder_.getPtrTy(), codegen_.builder_.CreateStructGEP(codegen_.promiseTy_, c.promise, kOut), "out");
    codegen_.builder_.CreateStore(value, out);
    codegen_.builder_.CreateStore(codegen_.builder_.getInt32(closable ? 1 : 0), close);
    codegen_.builder_.CreateBr(suspendBlock);
    codegen_.functionFrame().switchTo(dropBlock);
    codegen_.refCounts().releaseValue(value, c.yielded);
    codegen_.builder_.CreateBr(suspendBlock);
    codegen_.functionFrame().switchTo(suspendBlock);
    emitSuspend();
    if (closable) {
      // Resumed to close: a `return` from here.
      llvm::BasicBlock *closeBlock = codegen_.functionFrame().newBlock("yield.close");
      llvm::BasicBlock *goBlock = codegen_.functionFrame().newBlock("yield.resumed");
      codegen_.builder_.CreateCondBr(codegen_.builder_.CreateICmpEQ(codegen_.builder_.CreateLoad(codegen_.builder_.getInt32Ty(), close),
                                                  codegen_.builder_.getInt32(2)),
                            closeBlock, goBlock,
                            llvm::MDBuilder(codegen_.ctx_).createUnlikelyBranchWeights());
      codegen_.functionFrame().switchTo(closeBlock);
      codegen_.tryLowerer().emitReturnOf(nullptr);
      if (codegen_.functionFrame().blockIsOpen()) {
        codegen_.builder_.CreateUnreachable();
      }
      codegen_.functionFrame().switchTo(goBlock);
    }
  }
  if (!resultType) {
    return nullptr;
  }
  if (resultType->isUndefined()) {
    return codegen_.builder_.getFalse();
  }
  if (!c.sent) {
    return codegen_.optionalLowerer().undefinedValue(resultType);
  }
  // What next(x) sent, which the caller keeps: a copy for the generator.
  llvm::Value *in = codegen_.builder_.CreateLoad(codegen_.builder_.getPtrTy(),
                                        codegen_.builder_.CreateStructGEP(codegen_.promiseTy_, c.promise, kIn), "in");
  llvm::BasicBlock *before = codegen_.builder_.GetInsertBlock();
  llvm::BasicBlock *givenBlock = codegen_.functionFrame().newBlock("yield.sent");
  llvm::BasicBlock *endBlock = codegen_.functionFrame().newBlock("yield.end");
  llvm::BasicBlock *noneBlock = endBlock;
  if (!resultType->isOptional()) {
    noneBlock = codegen_.functionFrame().newBlock("yield.none");
  }
  codegen_.builder_.CreateCondBr(codegen_.builder_.CreateIsNotNull(in), givenBlock, noneBlock);
  if (!resultType->isOptional()) {
    // next() without a value resumed a `yield` whose value is needed.
    codegen_.functionFrame().switchTo(noneBlock);
    codegen_.throwLowerer().emitThrowNew(1, "next() resumed a 'yield' that needs a value of type '" +
                        resultType->name().str() + "' without one");
  }
  codegen_.functionFrame().switchTo(givenBlock);
  llvm::Value *sent = codegen_.builder_.CreateLoad(codegen_.moduleBuilder().lower(c.sent), in, "sent");
  codegen_.refCounts().retainValue(sent, c.sent);
  sent = codegen_.exprLowerer().emitConversion(sent, c.sent, resultType);
  llvm::BasicBlock *givenEnd = codegen_.builder_.GetInsertBlock();
  codegen_.builder_.CreateBr(endBlock);
  codegen_.functionFrame().switchTo(endBlock);
  llvm::PHINode *result = codegen_.builder_.CreatePHI(codegen_.moduleBuilder().lower(resultType), 2);
  result->addIncoming(sent, givenEnd);
  if (resultType->isOptional()) {
    result->addIncoming(codegen_.optionalLowerer().undefinedValue(resultType), before);
  }

  return result;
}

// The coroutine is done: what it returns goes where the caller wants it, if
// anywhere, and it waits at its final suspension to be destroyed.
void CoroutineLowerer::emitCoroutineReturn(llvm::Value *value, const Type *type) {
  CodeGen::Coroutine &c = *codegen_.coro_;
  if (value && type && !type->isVoid() && !type->isUndefined()) {
    llvm::Value *ret = codegen_.builder_.CreateLoad(
        codegen_.builder_.getPtrTy(), codegen_.builder_.CreateStructGEP(codegen_.promiseTy_, c.promise, kRet), "ret");
    llvm::BasicBlock *storeBlock = codegen_.functionFrame().newBlock("return.store");
    llvm::BasicBlock *dropBlock = codegen_.functionFrame().newBlock("return.drop");
    llvm::BasicBlock *doneBlock = codegen_.functionFrame().newBlock("return.done");
    codegen_.builder_.CreateCondBr(codegen_.builder_.CreateIsNotNull(ret), storeBlock, dropBlock);
    codegen_.functionFrame().switchTo(storeBlock);
    codegen_.builder_.CreateStore(value, ret);
    codegen_.builder_.CreateBr(doneBlock);
    codegen_.functionFrame().switchTo(dropBlock);
    codegen_.refCounts().releaseValue(value, type);
    codegen_.builder_.CreateBr(doneBlock);
    codegen_.functionFrame().switchTo(doneBlock);
  }
  codegen_.functionFrame().emitCleanups(0);
  if (codegen_.async_ && codegen_.async_->generator) {
    codegen_.asyncGeneratorLowerer().finishAsyncGenerator(nullptr);
  }
  codegen_.builder_.CreateBr(c.finalBlock);
  codegen_.functionFrame().startDeadBlock();
}

void CoroutineLowerer::finishCoroutine() {
  CodeGen::Coroutine &c = *codegen_.coro_;
  if (codegen_.functionFrame().blockIsOpen()) {
    codegen_.builder_.CreateUnreachable();
  }
  llvm::Value *none = llvm::ConstantTokenNone::get(codegen_.ctx_);
  // The final suspension; next() never resumes it from there.
  codegen_.functionFrame().switchTo(c.finalBlock);
  llvm::Value *state = codegen_.builder_.CreateCall(coroIntrinsic(llvm::Intrinsic::coro_suspend),
                                           {none, codegen_.builder_.getTrue()}, "final");
  llvm::BasicBlock *resumedBlock = codegen_.functionFrame().newBlock("coro.resumed.done");
  llvm::SwitchInst *next = codegen_.builder_.CreateSwitch(state, c.returnBlock, 2);
  next->addCase(codegen_.builder_.getInt8(0), resumedBlock);
  next->addCase(codegen_.builder_.getInt8(1), c.freeBlock);
  codegen_.functionFrame().switchTo(resumedBlock);
  codegen_.builder_.CreateUnreachable();

  // Freeing: the frame starts after the header of the object.
  codegen_.functionFrame().switchTo(c.freeBlock);
  llvm::Value *frame =
      codegen_.builder_.CreateCall(coroIntrinsic(llvm::Intrinsic::coro_free), {c.id, c.handle}, "mem");
  llvm::BasicBlock *freeObject = codegen_.functionFrame().newBlock("coro.free.object");
  codegen_.builder_.CreateCondBr(codegen_.builder_.CreateIsNotNull(frame), freeObject, c.returnBlock);
  codegen_.functionFrame().switchTo(freeObject);
  llvm::Value *back = llvm::ConstantInt::getSigned(codegen_.builder_.getInt64Ty(), -int64_t(c.header));
  codegen_.builder_.CreateCall(codegen_.rt_.objFree, {codegen_.builder_.CreateGEP(codegen_.builder_.getInt8Ty(), frame, back)});
  codegen_.builder_.CreateBr(c.returnBlock);

  codegen_.functionFrame().switchTo(c.returnBlock);
  codegen_.builder_.CreateCall(coroIntrinsic(llvm::Intrinsic::coro_end),
                      {c.handle, codegen_.builder_.getFalse(), none});
  codegen_.builder_.CreateRet(c.object);
}

llvm::Value *CoroutineLowerer::emitResume(llvm::Value *it, llvm::Value *out, llvm::Value *in,
                                 llvm::Value *ret) {
  llvm::Type *ptr = codegen_.builder_.getPtrTy();
  llvm::Value *frame =
      codegen_.builder_.CreateConstInBoundsGEP1_64(codegen_.builder_.getInt8Ty(), it, kHeaderSize, "frame");
  llvm::BasicBlock *before = codegen_.builder_.GetInsertBlock();
  llvm::BasicBlock *resumeBlock = codegen_.functionFrame().newBlock("next.resume");
  llvm::BasicBlock *endBlock = codegen_.functionFrame().newBlock("next.end");
  llvm::Value *done =
      codegen_.builder_.CreateCall(coroIntrinsic(llvm::Intrinsic::coro_done), {frame}, "done");
  codegen_.builder_.CreateCondBr(done, endBlock, resumeBlock);

  codegen_.functionFrame().switchTo(resumeBlock);
  llvm::Value *promise =
      codegen_.builder_.CreateCall(coroIntrinsic(llvm::Intrinsic::coro_promise),
                          {frame, codegen_.builder_.getInt32(kFrameAlign), codegen_.builder_.getFalse()}, "promise");
  // A generator that calls next() on itself, as JavaScript reports.
  llvm::Value *running = codegen_.builder_.CreateStructGEP(codegen_.promiseTy_, promise, kRunning);
  llvm::BasicBlock *runningBlock = codegen_.functionFrame().newBlock("next.running");
  llvm::BasicBlock *goBlock = codegen_.functionFrame().newBlock("next.go");
  codegen_.builder_.CreateCondBr(codegen_.builder_.CreateICmpNE(codegen_.builder_.CreateLoad(codegen_.builder_.getInt32Ty(), running),
                                              codegen_.builder_.getInt32(0)),
                        runningBlock, goBlock, llvm::MDBuilder(codegen_.ctx_).createUnlikelyBranchWeights());
  codegen_.functionFrame().switchTo(runningBlock);
  codegen_.throwLowerer().emitThrowNew(1, "Generator is already running");
  codegen_.functionFrame().switchTo(goBlock);
  llvm::Value *null = llvm::ConstantPointerNull::get(ptr);
  codegen_.builder_.CreateStore(out, codegen_.builder_.CreateStructGEP(codegen_.promiseTy_, promise, kOut));
  codegen_.builder_.CreateStore(in ? in : null, codegen_.builder_.CreateStructGEP(codegen_.promiseTy_, promise, kIn));
  codegen_.builder_.CreateStore(ret ? ret : null, codegen_.builder_.CreateStructGEP(codegen_.promiseTy_, promise, kRet));
  codegen_.builder_.CreateStore(codegen_.builder_.getInt32(1), running);
  codegen_.builder_.CreateCall(coroIntrinsic(llvm::Intrinsic::coro_resume), {frame});
  codegen_.builder_.CreateStore(codegen_.builder_.getInt32(0), running);
  // Its code threw: the generator is done, and the error goes on here.
  codegen_.throwLowerer().emitThrowCheck();
  llvm::Value *gave =
      codegen_.builder_.CreateNot(codegen_.builder_.CreateCall(coroIntrinsic(llvm::Intrinsic::coro_done), {frame}));
  llvm::BasicBlock *resumeEnd = codegen_.builder_.GetInsertBlock();
  codegen_.builder_.CreateBr(endBlock);

  codegen_.functionFrame().switchTo(endBlock);
  llvm::PHINode *has = codegen_.builder_.CreatePHI(codegen_.builder_.getInt1Ty(), 2, "has");
  has->addIncoming(codegen_.builder_.getFalse(), before);
  has->addIncoming(gave, resumeEnd);

  return has;
}

} // namespace tz
