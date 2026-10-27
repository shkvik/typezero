#include "compiler/lower/iterators/async_generator_lowerer/async_generator_lowerer.h"
#include "compiler/check/checker/checker.h"
#include "compiler/library/process/process_lowerer/process_lowerer.h"
#include "compiler/lower/async/async_lowerer/async_lowerer.h"
#include "compiler/lower/async/promise_lowerer/promise_lowerer.h"
#include "compiler/lower/expr/expr_lowerer/expr_lowerer.h"
#include "compiler/lower/function_builder/function_builder.h"
#include "compiler/lower/function_frame/function_frame.h"
#include "compiler/lower/functions/env_lowerer/env_lowerer.h"
#include "compiler/lower/iterators/coroutine_lowerer/coroutine_lowerer.h"
#include "compiler/lower/iterators/inline_generator_lowerer/inline_generator_lowerer.h"
#include "compiler/lower/layouts/type_descriptors/type_descriptors.h"
#include "compiler/lower/memory/ref_counts/ref_counts.h"
#include "compiler/lower/module_builder/module_builder.h"
#include "compiler/lower/stmt/exit_lowerer/exit_lowerer.h"
#include "compiler/lower/stmt/stmt_lowerer/stmt_lowerer.h"
#include "compiler/lower/types/optional_lowerer/optional_lowerer.h"
#include "llvm/IR/MDBuilder.h"

using llvm::BasicBlock;
using llvm::cast;
using llvm::dyn_cast;
using llvm::isa;
using llvm::Value;

namespace tz {

namespace {

constexpr uint64_t kAgenReactor = 40;

constexpr uint64_t kAgenHeader = 80;

constexpr uint32_t kAgenFrameAlign = 8;

constexpr uint32_t kLayoutAsyncGenerator = 268435456 | 536870912;

constexpr unsigned kPromiseFlags = 5;

constexpr uint32_t kPromiseDone = 8;

}

AsyncGeneratorLowerer::AsyncGeneratorLowerer(Lowering &lowering) : lowering_(lowering) {}

llvm::Constant *AsyncGeneratorLowerer::asyncGeneratorLayout() {
  if (agenLayout_) {
    return agenLayout_;
  }
  llvm::Type *ptr = lowering_.builder_.getPtrTy();
  auto *drop = llvm::Function::Create(
      llvm::FunctionType::get(lowering_.builder_.getVoidTy(), {ptr}, false),
      llvm::GlobalValue::InternalLinkage, "tz.agen.destroy", lowering_.module_.get());
  drop->addFnAttr(llvm::Attribute::NoUnwind);
  {
    llvm::IRBuilder<> b(llvm::BasicBlock::Create(lowering_.ctx_, "entry", drop));
    b.CreateCall(lowering_.asyncLowerer().asyncFunction("tz_agen_drop_steps", b.getVoidTy(), {ptr}),
                 {drop->getArg(0)});
    llvm::Value *frame =
        b.CreateConstInBoundsGEP1_64(b.getInt8Ty(), drop->getArg(0), kAgenHeader, "frame");
    b.CreateCall(lowering_.coroutineLowerer().coroIntrinsic(llvm::Intrinsic::coro_destroy),
                 {frame});
    b.CreateRetVoid();
  }
  auto callee = [&](llvm::StringRef name) {
    return cast<llvm::Constant>(lowering_.asyncLowerer()
                                    .asyncFunction(name, lowering_.builder_.getVoidTy(), {ptr, ptr})
                                    .getCallee());
  };
  llvm::Constant *null = llvm::ConstantPointerNull::get(lowering_.builder_.getPtrTy());
  auto *type = llvm::StructType::get(lowering_.ctx_, {lowering_.layoutDescTy_, ptr, ptr});
  llvm::Constant *base = llvm::ConstantStruct::get(
      lowering_.layoutDescTy_, {drop, null, lowering_.builder_.getInt32(0),
                                lowering_.builder_.getInt32(kLayoutAsyncGenerator), null,
                                lowering_.builder_.getInt32(0), lowering_.builder_.getInt32(0)});
  auto *global = new llvm::GlobalVariable(
      *lowering_.module_, type, true, llvm::GlobalValue::PrivateLinkage,
      llvm::ConstantStruct::get(type, {base, callee("tz_agen_next"), callee("tz_agen_close")}),
      "tz.layout.agen");
  global->setUnnamedAddr(llvm::GlobalValue::UnnamedAddr::Global);
  agenLayout_ = global;

  return agenLayout_;
}

void AsyncGeneratorLowerer::emitAsyncGenerator(FunctionDecl *fn, llvm::Function *function) {
  lowering_.functionBuilder().beginFunction(function, fn);
  Lowering::Coroutine coroutine{};
  coroutine.header = kAgenHeader;
  lowering_.coro_ = &coroutine;
  Lowering::AsyncState state{};
  state.generator = true;
  lowering_.async_ = &state;
  lowering_.function_->addFnAttr(llvm::Attribute::PresplitCoroutine);
  llvm::Type *i8 = lowering_.builder_.getInt8Ty(), *i64 = lowering_.builder_.getInt64Ty();
  llvm::Value *null = llvm::ConstantPointerNull::get(lowering_.builder_.getPtrTy());
  coroutine.id = lowering_.builder_.CreateCall(
      lowering_.coroutineLowerer().coroIntrinsic(llvm::Intrinsic::coro_id),
      {lowering_.builder_.getInt32(kAgenFrameAlign), null, null, null}, "id");
  llvm::Value *size = lowering_.builder_.CreateCall(
      lowering_.coroutineLowerer().coroIntrinsic(llvm::Intrinsic::coro_size, {i64}), {}, "size");
  llvm::Value *memory = lowering_.builder_.CreateCall(
      lowering_.rt_.objAlloc,
      {lowering_.builder_.CreateAdd(size, lowering_.builder_.getInt64(kAgenHeader))}, "memory");
  lowering_.builder_.CreateMemSet(memory, lowering_.builder_.getInt8(0), kAgenHeader,
                                  llvm::MaybeAlign(8));
  lowering_.builder_.CreateStore(lowering_.builder_.getInt64(1), memory);
  lowering_.builder_.CreateStore(asyncGeneratorLayout(),
                                 lowering_.builder_.CreateConstInBoundsGEP1_64(i8, memory, 8));
  llvm::Value *reactor = lowering_.builder_.CreateConstInBoundsGEP1_64(i8, memory, kAgenReactor);
  lowering_.builder_.CreateStore(lowering_.asyncLowerer().asyncResumeFunction(),
                                 lowering_.builder_.CreateConstInBoundsGEP1_64(i8, reactor, 8));
  lowering_.builder_.CreateStore(memory,
                                 lowering_.builder_.CreateConstInBoundsGEP1_64(i8, reactor, 16));
  llvm::Value *frame =
      lowering_.builder_.CreateConstInBoundsGEP1_64(i8, memory, kAgenHeader, "frame");
  coroutine.handle = lowering_.builder_.CreateCall(
      lowering_.coroutineLowerer().coroIntrinsic(llvm::Intrinsic::coro_begin),
      {coroutine.id, frame}, "handle");
  coroutine.object = lowering_.builder_.CreateGEP(
      i8, coroutine.handle, llvm::ConstantInt::getSigned(i64, -int64_t(kAgenHeader)), "generator");
  state.object = coroutine.object;
  state.reactor =
      lowering_.builder_.CreateConstInBoundsGEP1_64(i8, coroutine.object, kAgenReactor, "reactor");
  coroutine.finalBlock = lowering_.functionFrame().newBlock("coro.final");
  coroutine.freeBlock = lowering_.functionFrame().newBlock("coro.free");
  coroutine.returnBlock = lowering_.functionFrame().newBlock("coro.return");

  lowering_.functionFrame().pushScope();
  std::vector<llvm::Value *> args;
  unsigned first = 0;
  if (fn->isClosure() || fn->takesThis()) {
    lowering_.envParam_ = lowering_.function_->getArg(0);
    if (fn->isClosure()) {
      lowering_.envLowerer().bindCaptures(fn);
    } else {
      lowering_.envParam_->setName("this");
    }
    llvm::Value *slot = lowering_.functionFrame().createObjectSlot("env");
    lowering_.builder_.CreateCall(lowering_.objRetainFn_, {lowering_.envParam_});
    lowering_.builder_.CreateStore(lowering_.envParam_, slot);
    first = 1;
  }
  for (unsigned i = first; i < lowering_.function_->arg_size(); ++i) {
    args.push_back(lowering_.function_->getArg(i));
  }
  lowering_.functionBuilder().emitParams(fn, args);
  lowering_.coroutineLowerer().emitSuspend();
  lowering_.stmtLowerer().emitStmt(fn->body);
  if (lowering_.functionFrame().blockIsOpen()) {
    lowering_.coroutineLowerer().emitCoroutineReturn(nullptr, nullptr);
  }
  lowering_.scopes_.pop_back();
  lowering_.coroutineLowerer().finishCoroutine();
  lowering_.coro_ = nullptr;
  lowering_.async_ = nullptr;
  lowering_.function_ = nullptr;
}

void AsyncGeneratorLowerer::finishAsyncGenerator() {
  lowering_.builder_.CreateCall(
      lowering_.asyncLowerer().asyncFunction("tz_agen_finish", lowering_.builder_.getVoidTy(),
                                             {lowering_.builder_.getPtrTy()}),
      {lowering_.async_->object});
}

void AsyncGeneratorLowerer::emitAsyncGeneratorReturn(ReturnStmt *stmt) {
  if (stmt->value) {
    const Type *type = stmt->value->type;
    llvm::Value *value =
        awaitValueOf(lowering_.exprLowerer().emitExpr(stmt->value), type, stmt->loc());
    const Type *given = Checker::awaitedTypeOf(type);
    if (value && !given->isVoid()) {
      lowering_.refCounts().releaseValue(value, given);
    }
  }
  lowering_.exitLowerer().emitReturnOf(nullptr);
}

llvm::Value *AsyncGeneratorLowerer::awaitValueOf(llvm::Value *value, const Type *type,
                                                 llvm::SMLoc loc) {
  if (type->isPromise()) {
    return lowering_.asyncLowerer().awaitPromise(value, type, loc);
  }
  if (AsyncLowerer::holdsPromise(type)) {
    return lowering_.asyncLowerer().awaitUnion(value, type, Checker::awaitedTypeOf(type), loc);
  }
  lowering_.asyncLowerer().suspendAsync(nullptr, loc);

  return value;
}

void AsyncGeneratorLowerer::agenYield(llvm::Value *value, const Type *type, llvm::SMLoc loc) {
  llvm::Type *ptr = lowering_.builder_.getPtrTy(), *i32 = lowering_.builder_.getInt32Ty();
  llvm::Value *slot = value && lowering_.promiseLowerer().promiseValueSize(type)
                          ? lowering_.functionFrame().spill(value, "yielded")
                          : static_cast<llvm::Value *>(llvm::ConstantPointerNull::get(ptr));
  if (value && !lowering_.promiseLowerer().promiseValueSize(type)) {
    lowering_.refCounts().releaseValue(value, type);
  }
  llvm::Value *descriptor =
      lowering_.promiseLowerer().promiseValueSize(type)
          ? static_cast<llvm::Value *>(lowering_.typeDescriptors().typeDescriptor(type))
          : llvm::ConstantPointerNull::get(ptr);
  llvm::Value *next = lowering_.builder_.CreateCall(
      lowering_.asyncLowerer().asyncFunction("tz_agen_yield", i32, {ptr, ptr, ptr}),
      {lowering_.async_->object, slot, descriptor}, "next");
  llvm::BasicBlock *waitBlock = lowering_.functionFrame().newBlock("yield.wait");
  llvm::BasicBlock *decideBlock = lowering_.functionFrame().newBlock("yield.decide");
  llvm::BasicBlock *closeBlock = lowering_.functionFrame().newBlock("yield.close");
  llvm::BasicBlock *goBlock = lowering_.functionFrame().newBlock("yield.go");
  llvm::BasicBlock *before = lowering_.builder_.GetInsertBlock();
  lowering_.builder_.CreateCondBr(
      lowering_.builder_.CreateICmpSLT(next, lowering_.builder_.getInt32(0)), waitBlock,
      decideBlock);
  lowering_.functionFrame().switchTo(waitBlock);
  lowering_.coroutineLowerer().emitSuspend();
  llvm::Value *resumed = lowering_.builder_.CreateCall(
      lowering_.asyncLowerer().asyncFunction("tz_agen_resumed", i32, {ptr}),
      {lowering_.async_->object}, "resumed");
  llvm::BasicBlock *waited = lowering_.builder_.GetInsertBlock();
  lowering_.builder_.CreateBr(decideBlock);
  lowering_.functionFrame().switchTo(decideBlock);
  llvm::PHINode *how = lowering_.builder_.CreatePHI(i32, 2, "how");
  how->addIncoming(next, before);
  how->addIncoming(resumed, waited);
  lowering_.builder_.CreateCondBr(
      lowering_.builder_.CreateICmpEQ(how, lowering_.builder_.getInt32(1)), closeBlock, goBlock,
      llvm::MDBuilder(lowering_.ctx_).createUnlikelyBranchWeights());
  lowering_.functionFrame().switchTo(closeBlock);
  lowering_.asyncLowerer().suspendAsync(nullptr, loc);
  lowering_.exitLowerer().emitReturnOf(nullptr);
  if (lowering_.functionFrame().blockIsOpen()) {
    lowering_.builder_.CreateUnreachable();
  }
  lowering_.functionFrame().switchTo(goBlock);
}

llvm::Value *AsyncGeneratorLowerer::emitAsyncYield(YieldExpr *yield) {
  const Type *yielded = lowering_.currentFn_->sym->ret->yielded();
  if (yield->delegate) {
    emitAsyncYieldAll(yield, yielded);
    return lowering_.builder_.getFalse();
  }
  llvm::Value *value = nullptr;
  const Type *given = Type::undefinedTy();
  if (yield->value) {
    const Type *type = yield->value->type;
    value = awaitValueOf(lowering_.exprLowerer().emitExpr(yield->value), type, yield->loc());
    given = Checker::awaitedTypeOf(type);
  } else {
    lowering_.asyncLowerer().suspendAsync(nullptr, yield->loc());
  }
  llvm::Value *converted = nullptr;
  if (value && !given->isVoid() && !given->isUndefined()) {
    converted = lowering_.exprLowerer().emitConversion(value, given, yielded);
  } else if (!yielded->isUndefined() && yielded->isOptional()) {
    converted = lowering_.optionalLowerer().undefinedValue(yielded);
  }
  agenYield(converted, yielded, yield->loc());

  return lowering_.builder_.getFalse();
}

llvm::Value *AsyncGeneratorLowerer::emitAsyncStep(llvm::Value *slot, const Type *element,
                                                  llvm::SMLoc loc, llvm::BasicBlock *endBlock) {
  llvm::Type *ptr = lowering_.builder_.getPtrTy(), *i32 = lowering_.builder_.getInt32Ty();
  llvm::Value *it = lowering_.builder_.CreateLoad(ptr, slot, "iterator");
  llvm::Value *step = lowering_.promiseLowerer().newPromise(element);
  lowering_.builder_.CreateCall(
      lowering_.asyncLowerer().asyncFunction("tz_async_iter_next", lowering_.builder_.getVoidTy(),
                                             {ptr, ptr}),
      {it, step});
  lowering_.asyncLowerer().suspendAsync(step, loc);
  auto forget = [&] {
    llvm::Value *held = lowering_.builder_.CreateLoad(ptr, slot, "iterator");
    lowering_.builder_.CreateStore(llvm::ConstantPointerNull::get(ptr), slot);
    lowering_.builder_.CreateCall(lowering_.instanceReleaseFn_, {held});
  };
  llvm::Value *flags = lowering_.builder_.CreateLoad(
      i32, lowering_.promiseLowerer().promiseField(step, kPromiseFlags), "flags");
  llvm::BasicBlock *doneBlock = lowering_.functionFrame().newBlock("for.await.done");
  llvm::BasicBlock *valueBlock = lowering_.functionFrame().newBlock("for.await.value");
  lowering_.builder_.CreateCondBr(
      lowering_.builder_.CreateICmpNE(
          lowering_.builder_.CreateAnd(flags, lowering_.builder_.getInt32(kPromiseDone)),
          lowering_.builder_.getInt32(0)),
      doneBlock, valueBlock);
  lowering_.functionFrame().switchTo(doneBlock);
  lowering_.builder_.CreateCall(lowering_.instanceReleaseFn_, {step});
  forget();
  lowering_.builder_.CreateBr(endBlock);
  lowering_.functionFrame().switchTo(valueBlock);
  llvm::Value *value;
  if (lowering_.promiseLowerer().promiseValueSize(element)) {
    value = lowering_.builder_.CreateLoad(lowering_.moduleBuilder().lower(element),
                                          lowering_.promiseLowerer().promiseValueAddress(step),
                                          "value");
    lowering_.refCounts().retainValue(value, element);
  } else {
    value = element->isUndefined() ? static_cast<llvm::Value *>(lowering_.builder_.getFalse())
                                   : lowering_.optionalLowerer().undefinedValue(element);
  }
  lowering_.builder_.CreateCall(lowering_.instanceReleaseFn_, {step});

  return value;
}

void AsyncGeneratorLowerer::closeAsyncIterator(llvm::Value *slot) {
  llvm::Type *ptr = lowering_.builder_.getPtrTy();
  llvm::Value *it = lowering_.builder_.CreateLoad(ptr, slot, "iterator");
  llvm::BasicBlock *closeBlock = lowering_.functionFrame().newBlock("for.await.close");
  llvm::BasicBlock *nextBlock = lowering_.functionFrame().newBlock("for.await.closed");
  lowering_.builder_.CreateCondBr(lowering_.builder_.CreateIsNotNull(it), closeBlock, nextBlock);
  lowering_.functionFrame().switchTo(closeBlock);
  if (lowering_.cleanupNoAwait_ || !lowering_.async_) {
    lowering_.builder_.CreateBr(nextBlock);
    lowering_.functionFrame().switchTo(nextBlock);
    return;
  }
  llvm::Value *closed = lowering_.promiseLowerer().newPromise(Type::voidTy());
  lowering_.builder_.CreateCall(
      lowering_.asyncLowerer().asyncFunction("tz_async_iter_close", lowering_.builder_.getVoidTy(),
                                             {ptr, ptr}),
      {it, closed});
  lowering_.asyncLowerer().suspendAsync(closed, lowering_.loc_);
  lowering_.builder_.CreateCall(lowering_.instanceReleaseFn_, {closed});
  lowering_.builder_.CreateBr(nextBlock);
  lowering_.functionFrame().switchTo(nextBlock);
}

void AsyncGeneratorLowerer::emitAsyncYieldAll(YieldExpr *yield, const Type *yielded) {
  const Type *type = yield->value->type;
  const Type *element = yield->element;
  lowering_.functionFrame().pushScope();
  if (type->isAsyncIterator() || type->isRuntimeObject()) {
    llvm::Value *slot = lowering_.functionFrame().createManagedSlot(type, "yield.iterator");
    lowering_.builder_.CreateStore(lowering_.exprLowerer().emitExpr(yield->value), slot);
    lowering_.scopes_.back().asyncIterators.push_back(slot);
    llvm::BasicBlock *headBlock = lowering_.functionFrame().newBlock("yield.all.next");
    llvm::BasicBlock *endBlock = lowering_.functionFrame().newBlock("yield.all.end");
    lowering_.functionFrame().emitBlock(headBlock);
    llvm::Value *value = emitAsyncStep(slot, element, yield->loc(), endBlock);
    agenYield(lowering_.exprLowerer().emitConversion(value, element, yielded), yielded,
              yield->loc());
    lowering_.builder_.CreateBr(headBlock);
    lowering_.functionFrame().switchTo(endBlock);
    lowering_.functionFrame().popScope();
    return;
  }
  Lowering::Producer producer = lowering_.inlineGeneratorLowerer().prepare(yield->value);
  size_t depth = lowering_.scopes_.size();
  llvm::BasicBlock *endBlock = lowering_.functionFrame().newBlock("yield.all.end");
  const Type *from = type->isArray() ? type->inner() : type->isString() ? type : type->yielded();
  llvm::SMLoc loc = yield->loc();
  Lowering::Consumer consumer;
  consumer.stop = [this, depth, endBlock] {
    lowering_.functionFrame().emitCleanups(depth);
    lowering_.builder_.CreateBr(endBlock);
  };
  consumer.each = [this, from, element, yielded, loc](llvm::Value *value) -> llvm::Value * {
    llvm::Value *awaited = awaitValueOf(value, from, loc);
    lowering_.asyncLowerer().suspendAsync(nullptr, loc);
    agenYield(lowering_.exprLowerer().emitConversion(awaited, element, yielded), yielded, loc);

    return nullptr;
  };
  producer.run(consumer);
  lowering_.functionFrame().emitBlock(endBlock);
  lowering_.functionFrame().popScope();
}

void AsyncGeneratorLowerer::emitForAwait(ForOfStmt *stmt) {
  llvm::SmallVector<llvm::StringRef, 1> labels = std::move(lowering_.pendingLabels_);
  lowering_.pendingLabels_.clear();
  const Type *type = stmt->iterable->type;
  const Type *element = stmt->binding.sym->type;
  lowering_.functionFrame().pushScope();
  if (type->isAsyncIterator() || type->isRuntimeObject()) {
    llvm::Value *slot = lowering_.functionFrame().createManagedSlot(type, "for.await.iterator");
    lowering_.builder_.CreateStore(lowering_.exprLowerer().emitExpr(stmt->iterable), slot);
    lowering_.scopes_.back().asyncIterators.push_back(slot);
    size_t depth = lowering_.scopes_.size();
    llvm::BasicBlock *headBlock = lowering_.functionFrame().newBlock("for.await.next");
    llvm::BasicBlock *endBlock = lowering_.functionFrame().newBlock("for.await.end");
    lowering_.functionFrame().emitBlock(headBlock);
    llvm::Value *value = emitAsyncStep(slot, element, stmt->loc(), endBlock);
    lowering_.functionFrame().pushScope();
    lowering_.stmtLowerer().storeReplacing(
        lowering_.functionFrame().createLocal(stmt->binding.sym, stmt->binding.name), value,
        element);
    lowering_.targets_.push_back({labels, true, false, endBlock, headBlock, depth});
    lowering_.stmtLowerer().emitStmt(stmt->body);
    lowering_.targets_.pop_back();
    lowering_.functionFrame().popScope();
    lowering_.functionFrame().branchTo(headBlock);
    lowering_.functionFrame().switchTo(endBlock);
    lowering_.functionFrame().popScope();
    return;
  }
  Lowering::Producer producer = lowering_.inlineGeneratorLowerer().prepare(stmt->iterable);
  size_t depth = lowering_.scopes_.size();
  llvm::BasicBlock *endBlock = lowering_.functionFrame().newBlock("for.await.end");
  const Type *from = type->isArray() ? type->inner() : type->isString() ? type : type->yielded();
  Lowering::Consumer consumer;
  consumer.stop = [this, depth, endBlock] {
    lowering_.functionFrame().emitCleanups(depth);
    lowering_.builder_.CreateBr(endBlock);
  };
  consumer.each = [this, stmt, labels, depth, endBlock, from,
                   element](llvm::Value *value) -> llvm::Value * {
    llvm::Value *awaited = awaitValueOf(value, from, stmt->loc());
    lowering_.asyncLowerer().suspendAsync(nullptr, stmt->loc());
    const Type *given = Checker::awaitedTypeOf(from);
    llvm::Value *converted = element->isUndefined() ? lowering_.builder_.getFalse()
                             : given->isVoid()
                                 ? lowering_.optionalLowerer().undefinedValue(element)
                                 : lowering_.exprLowerer().emitConversion(awaited, given, element);
    if (element->isUndefined() && awaited && !given->isVoid()) {
      lowering_.refCounts().releaseValue(awaited, given);
    }
    llvm::BasicBlock *nextBlock = lowering_.functionFrame().newBlock("for.await.next");
    size_t here = lowering_.scopes_.size();
    lowering_.functionFrame().pushScope();
    lowering_.stmtLowerer().storeReplacing(
        lowering_.functionFrame().createLocal(stmt->binding.sym, stmt->binding.name), converted,
        element);
    lowering_.targets_.push_back({labels, true, false, endBlock, nextBlock, depth, here});
    lowering_.stmtLowerer().emitStmt(stmt->body);
    lowering_.targets_.pop_back();
    lowering_.functionFrame().popScope();
    lowering_.functionFrame().emitBlock(nextBlock);

    return nullptr;
  };
  producer.run(consumer);
  lowering_.functionFrame().emitBlock(endBlock);
  lowering_.functionFrame().popScope();
}

}
