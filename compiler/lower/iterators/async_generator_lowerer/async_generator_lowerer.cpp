#include "compiler/lower/iterators/async_generator_lowerer/async_generator_lowerer.h"
#include "compiler/lower/module_builder/module_builder.h"
#include "compiler/lower/function_builder/function_builder.h"
#include "compiler/lower/function_frame/function_frame.h"
#include "compiler/lower/memory/ref_counts/ref_counts.h"
#include "compiler/lower/types/optional_lowerer/optional_lowerer.h"
#include "compiler/lower/stmt/stmt_lowerer/stmt_lowerer.h"
#include "compiler/lower/expr/expr_lowerer/expr_lowerer.h"
#include "compiler/lower/layouts/type_descriptors/type_descriptors.h"
#include "compiler/lower/async/promise_lowerer/promise_lowerer.h"
#include "compiler/lower/async/async_lowerer/async_lowerer.h"
#include "compiler/lower/functions/env_lowerer/env_lowerer.h"
#include "compiler/lower/iterators/coroutine_lowerer/coroutine_lowerer.h"
#include "compiler/lower/iterators/inline_generator_lowerer/inline_generator_lowerer.h"
#include "compiler/lower/errors/throw_lowerer/throw_lowerer.h"
#include "compiler/lower/errors/unwind_lowerer/unwind_lowerer.h"
#include "compiler/lower/errors/try_lowerer/try_lowerer.h"
#include "compiler/library/process/process_lowerer/process_lowerer.h"
#include "compiler/check/sema/sema.h"
#include "llvm/IR/MDBuilder.h"

using llvm::cast;
using llvm::dyn_cast;
using llvm::isa;
using llvm::BasicBlock;
using llvm::Value;

namespace tz {

namespace {

// Must match TzAsyncGen in runtime/core/tzrt/tzrt.h: the reactor, then the
// frame.
constexpr uint64_t kAgenReactor = 40;

constexpr uint64_t kAgenHeader = 80;

constexpr uint32_t kAgenFrameAlign = 8;

// TZ_LAYOUT_ASYNC_ITERATOR | TZ_LAYOUT_ASYNC_GENERATOR
constexpr uint32_t kLayoutAsyncGenerator = 268435456 | 536870912;

// TzPromise: { rc, layout, head, tail, error, i32 state, i32 flags }
constexpr unsigned kPromiseState = 5, kPromiseFlags = 6, kPromiseError = 4;

constexpr uint32_t kPromiseFulfilled = 1;

constexpr uint32_t kPromiseDone = 8; // TZ_PROMISE_DONE

} // namespace

AsyncGeneratorLowerer::AsyncGeneratorLowerer(CodeGen &codegen) : codegen_(codegen) {}

// The layout of async generators: the drop function, which destroys the
// coroutine where it waits (without awaiting anything, as JavaScript drops
// one), and what `for await` calls.
llvm::Constant *AsyncGeneratorLowerer::asyncGeneratorLayout() {
  if (agenLayout_) {
    return agenLayout_;
  }
  llvm::Type *ptr = codegen_.builder_.getPtrTy();
  auto *drop =
      llvm::Function::Create(llvm::FunctionType::get(codegen_.builder_.getVoidTy(), {ptr}, false),
                             llvm::GlobalValue::InternalLinkage, "tz.agen.destroy", codegen_.module_.get());
  drop->addFnAttr(llvm::Attribute::NoUnwind);
  {
    llvm::IRBuilder<> b(llvm::BasicBlock::Create(codegen_.ctx_, "entry", drop));
    b.CreateCall(codegen_.asyncLowerer().asyncFunction("tz_agen_drop_steps", b.getVoidTy(), {ptr}), {drop->getArg(0)});
    llvm::Value *frame =
        b.CreateConstInBoundsGEP1_64(b.getInt8Ty(), drop->getArg(0), kAgenHeader, "frame");
    b.CreateCall(codegen_.coroutineLowerer().coroIntrinsic(llvm::Intrinsic::coro_destroy), {frame});
    b.CreateRetVoid();
  }
  auto callee = [&](llvm::StringRef name) {
    return cast<llvm::Constant>(codegen_.asyncLowerer().asyncFunction(name, codegen_.builder_.getVoidTy(), {ptr, ptr}).getCallee());
  };
  llvm::Constant *null = llvm::ConstantPointerNull::get(codegen_.builder_.getPtrTy());
  auto *type = llvm::StructType::get(codegen_.ctx_, {codegen_.layoutDescTy_, ptr, ptr});
  llvm::Constant *base = llvm::ConstantStruct::get(
      codegen_.layoutDescTy_, {drop, null, codegen_.builder_.getInt32(0), codegen_.builder_.getInt32(kLayoutAsyncGenerator),
                      null, codegen_.builder_.getInt32(0), codegen_.builder_.getInt32(0)});
  auto *global = new llvm::GlobalVariable(
      *codegen_.module_, type, /*isConstant=*/true, llvm::GlobalValue::PrivateLinkage,
      llvm::ConstantStruct::get(type, {base, callee("tz_agen_next"), callee("tz_agen_close")}),
      "tz.layout.agen");
  global->setUnnamedAddr(llvm::GlobalValue::UnnamedAddr::Global);
  agenLayout_ = global;

  return agenLayout_;
}

// An async generator function: a call binds the parameters and returns the
// generator, whose body starts with the first step asked of it.
void AsyncGeneratorLowerer::emitAsyncGenerator(FunctionDecl *fn, llvm::Function *function) {
  codegen_.functionBuilder().beginFunction(function, fn);
  CodeGen::Coroutine coroutine{};
  coroutine.header = kAgenHeader;
  codegen_.coro_ = &coroutine;
  CodeGen::AsyncState state{};
  state.generator = true;
  codegen_.async_ = &state;
  codegen_.function_->addFnAttr(llvm::Attribute::PresplitCoroutine);
  llvm::Type *i8 = codegen_.builder_.getInt8Ty(), *i64 = codegen_.builder_.getInt64Ty();
  llvm::Value *null = llvm::ConstantPointerNull::get(codegen_.builder_.getPtrTy());
  coroutine.id = codegen_.builder_.CreateCall(codegen_.coroutineLowerer().coroIntrinsic(llvm::Intrinsic::coro_id),
                                     {codegen_.builder_.getInt32(kAgenFrameAlign), null, null, null}, "id");
  llvm::Value *size =
      codegen_.builder_.CreateCall(codegen_.coroutineLowerer().coroIntrinsic(llvm::Intrinsic::coro_size, {i64}), {}, "size");
  llvm::Value *memory = codegen_.builder_.CreateCall(
      codegen_.rt_.objAlloc, {codegen_.builder_.CreateAdd(size, codegen_.builder_.getInt64(kAgenHeader))}, "memory");
  codegen_.builder_.CreateMemSet(memory, codegen_.builder_.getInt8(0), kAgenHeader, llvm::MaybeAlign(8));
  codegen_.builder_.CreateStore(codegen_.builder_.getInt64(1), memory);
  codegen_.builder_.CreateStore(asyncGeneratorLayout(), codegen_.builder_.CreateConstInBoundsGEP1_64(i8, memory, 8));
  llvm::Value *reactor = codegen_.builder_.CreateConstInBoundsGEP1_64(i8, memory, kAgenReactor);
  codegen_.builder_.CreateStore(codegen_.asyncLowerer().asyncResumeFunction(), codegen_.builder_.CreateConstInBoundsGEP1_64(i8, reactor, 8));
  codegen_.builder_.CreateStore(memory, codegen_.builder_.CreateConstInBoundsGEP1_64(i8, reactor, 16));
  llvm::Value *frame = codegen_.builder_.CreateConstInBoundsGEP1_64(i8, memory, kAgenHeader, "frame");
  coroutine.handle = codegen_.builder_.CreateCall(codegen_.coroutineLowerer().coroIntrinsic(llvm::Intrinsic::coro_begin),
                                         {coroutine.id, frame}, "handle");
  // As for a generator, the object comes from the handle: LLVM takes the
  // frame to be reached only through it.
  coroutine.object = codegen_.builder_.CreateGEP(
      i8, coroutine.handle, llvm::ConstantInt::getSigned(i64, -int64_t(kAgenHeader)), "generator");
  state.object = coroutine.object;
  state.reactor =
      codegen_.builder_.CreateConstInBoundsGEP1_64(i8, coroutine.object, kAgenReactor, "reactor");
  coroutine.finalBlock = codegen_.functionFrame().newBlock("coro.final");
  coroutine.freeBlock = codegen_.functionFrame().newBlock("coro.free");
  coroutine.returnBlock = codegen_.functionFrame().newBlock("coro.return");

  codegen_.functionFrame().pushScope(); // the parameters, and the environment of a closure
  std::vector<llvm::Value *> args;
  unsigned first = 0;
  if (fn->isClosure() || fn->takesThis()) {
    codegen_.envParam_ = codegen_.function_->getArg(0);
    if (fn->isClosure()) {
      codegen_.envLowerer().bindCaptures(fn);
    } else {
      codegen_.envParam_->setName("this");
    }
    llvm::Value *slot = codegen_.functionFrame().createObjectSlot("env");
    codegen_.builder_.CreateCall(codegen_.objRetainFn_, {codegen_.envParam_});
    codegen_.builder_.CreateStore(codegen_.envParam_, slot);
    first = 1;
  }
  for (unsigned i = first; i < codegen_.function_->arg_size(); ++i) {
    args.push_back(codegen_.function_->getArg(i));
  }
  codegen_.functionBuilder().emitParams(fn, args);
  codegen_.coroutineLowerer().emitSuspend(); // until the first step
  codegen_.stmtLowerer().emitStmt(fn->body);
  if (codegen_.functionFrame().blockIsOpen()) {
    codegen_.coroutineLowerer().emitCoroutineReturn(nullptr, nullptr);
  }
  codegen_.unwindLowerer().finishScopeUnwind(0);
  codegen_.scopes_.pop_back(); // every way out released it
  codegen_.unwindLowerer().finishFunctionUnwind();
  codegen_.coroutineLowerer().finishCoroutine();
  codegen_.coro_ = nullptr;
  codegen_.async_ = nullptr;
  codegen_.function_ = nullptr;
}

// The body is done: the step it ran for gets done, then those waiting.
void AsyncGeneratorLowerer::finishAsyncGenerator(llvm::Value *error) {
  llvm::Type *ptr = codegen_.builder_.getPtrTy();
  codegen_.builder_.CreateCall(codegen_.asyncLowerer().asyncFunction("tz_agen_finish", codegen_.builder_.getVoidTy(), {ptr, ptr}),
                      {codegen_.async_->object, error ? error : llvm::ConstantPointerNull::get(ptr)});
}

void AsyncGeneratorLowerer::emitAsyncGeneratorReturn(ReturnStmt *stmt) {
  if (stmt->value) {
    const Type *type = stmt->value->type;
    llvm::Value *value = awaitValueOf(codegen_.exprLowerer().emitExpr(stmt->value), type, stmt->loc());
    const Type *given = Sema::awaitedTypeOf(type);
    if (value && !given->isVoid()) {
      codegen_.refCounts().releaseValue(value, given);
    }
  }
  codegen_.tryLowerer().emitReturnOf(nullptr);
}

llvm::Value *AsyncGeneratorLowerer::awaitValueOf(llvm::Value *value, const Type *type, llvm::SMLoc loc) {
  if (type->isPromise()) {
    return codegen_.asyncLowerer().awaitPromise(value, type, loc);
  }
  if (AsyncLowerer::holdsPromise(type)) {
    return codegen_.asyncLowerer().awaitUnion(value, type, Sema::awaitedTypeOf(type), loc);
  }
  codegen_.asyncLowerer().suspendAsync(nullptr, loc);

  return value;
}

// AsyncGeneratorYield: `value` (owned, of `type`, null for none) settles
// the step that runs, and the body goes on with what comes next: the next
// step at once, or later; or return() returns from here, after a turn.
void AsyncGeneratorLowerer::agenYield(llvm::Value *value, const Type *type, llvm::SMLoc loc) {
  llvm::Type *ptr = codegen_.builder_.getPtrTy(), *i32 = codegen_.builder_.getInt32Ty();
  llvm::Value *slot = value && codegen_.promiseLowerer().promiseValueSize(type)
                          ? codegen_.functionFrame().spill(value, "yielded")
                          : static_cast<llvm::Value *>(llvm::ConstantPointerNull::get(ptr));
  if (value && !codegen_.promiseLowerer().promiseValueSize(type)) {
    codegen_.refCounts().releaseValue(value, type);
  }
  llvm::Value *descriptor = codegen_.promiseLowerer().promiseValueSize(type)
                                ? static_cast<llvm::Value *>(codegen_.typeDescriptors().typeDescriptor(type))
                                : llvm::ConstantPointerNull::get(ptr);
  llvm::Value *next = codegen_.builder_.CreateCall(codegen_.asyncLowerer().asyncFunction("tz_agen_yield", i32, {ptr, ptr, ptr}),
                                          {codegen_.async_->object, slot, descriptor}, "next");
  llvm::BasicBlock *waitBlock = codegen_.functionFrame().newBlock("yield.wait");
  llvm::BasicBlock *decideBlock = codegen_.functionFrame().newBlock("yield.decide");
  llvm::BasicBlock *closeBlock = codegen_.functionFrame().newBlock("yield.close");
  llvm::BasicBlock *goBlock = codegen_.functionFrame().newBlock("yield.go");
  llvm::BasicBlock *before = codegen_.builder_.GetInsertBlock();
  codegen_.builder_.CreateCondBr(codegen_.builder_.CreateICmpSLT(next, codegen_.builder_.getInt32(0)), waitBlock, decideBlock);
  codegen_.functionFrame().switchTo(waitBlock);
  codegen_.coroutineLowerer().emitSuspend();
  llvm::Value *resumed = codegen_.builder_.CreateCall(codegen_.asyncLowerer().asyncFunction("tz_agen_resumed", i32, {ptr}),
                                             {codegen_.async_->object}, "resumed");
  llvm::BasicBlock *waited = codegen_.builder_.GetInsertBlock();
  codegen_.builder_.CreateBr(decideBlock);
  codegen_.functionFrame().switchTo(decideBlock);
  llvm::PHINode *how = codegen_.builder_.CreatePHI(i32, 2, "how");
  how->addIncoming(next, before);
  how->addIncoming(resumed, waited);
  codegen_.builder_.CreateCondBr(codegen_.builder_.CreateICmpEQ(how, codegen_.builder_.getInt32(1)), closeBlock, goBlock,
                        llvm::MDBuilder(codegen_.ctx_).createUnlikelyBranchWeights());
  // return(): AsyncGeneratorUnwrapYieldResumption awaits the value it
  // returns, then returns from here.
  codegen_.functionFrame().switchTo(closeBlock);
  codegen_.asyncLowerer().suspendAsync(nullptr, loc);
  codegen_.tryLowerer().emitReturnOf(nullptr);
  if (codegen_.functionFrame().blockIsOpen()) {
    codegen_.builder_.CreateUnreachable();
  }
  codegen_.functionFrame().switchTo(goBlock);
}

llvm::Value *AsyncGeneratorLowerer::emitAsyncYield(YieldExpr *yield) {
  const Type *yielded = codegen_.currentFn_->sym->ret->yielded();
  if (yield->delegate) {
    emitAsyncYieldAll(yield, yielded);
    return codegen_.builder_.getFalse(); // undefined
  }
  // `yield value` awaits the value first.
  llvm::Value *value = nullptr;
  const Type *given = Type::undefinedTy();
  if (yield->value) {
    const Type *type = yield->value->type;
    value = awaitValueOf(codegen_.exprLowerer().emitExpr(yield->value), type, yield->loc());
    given = Sema::awaitedTypeOf(type);
  } else {
    codegen_.asyncLowerer().suspendAsync(nullptr, yield->loc());
  }
  llvm::Value *converted = nullptr;
  if (value && !given->isVoid() && !given->isUndefined()) {
    converted = codegen_.exprLowerer().emitConversion(value, given, yielded);
  } else if (!yielded->isUndefined() && yielded->isOptional()) {
    converted = codegen_.optionalLowerer().undefinedValue(yielded);
  }
  agenYield(converted, yielded, yield->loc());

  return codegen_.builder_.getFalse(); // what next() sent: undefined
}

// One step of the async iterator held in `slot`, of type `type`: the value
// it gives (owned, of `element`), or on to `endBlock` once it is done. The
// iterator that is done, or whose step was rejected, is not closed.
llvm::Value *AsyncGeneratorLowerer::emitAsyncStep(llvm::Value *slot, const Type *element, llvm::SMLoc loc,
                                    llvm::BasicBlock *endBlock) {
  llvm::Type *ptr = codegen_.builder_.getPtrTy(), *i32 = codegen_.builder_.getInt32Ty();
  llvm::Value *it = codegen_.builder_.CreateLoad(ptr, slot, "iterator");
  llvm::Value *step = codegen_.promiseLowerer().newPromise(element);
  codegen_.builder_.CreateCall(codegen_.asyncLowerer().asyncFunction("tz_async_iter_next", codegen_.builder_.getVoidTy(), {ptr, ptr}),
                      {it, step});
  codegen_.asyncLowerer().suspendAsync(step, loc);
  // Let go of the iterator without closing it.
  auto forget = [&] {
    llvm::Value *held = codegen_.builder_.CreateLoad(ptr, slot, "iterator");
    codegen_.builder_.CreateStore(llvm::ConstantPointerNull::get(ptr), slot);
    codegen_.builder_.CreateCall(codegen_.instanceReleaseFn_, {held});
  };
  llvm::Value *state = codegen_.builder_.CreateLoad(i32, codegen_.promiseLowerer().promiseField(step, kPromiseState), "state");
  llvm::BasicBlock *fulfilledBlock = codegen_.functionFrame().newBlock("for.await.fulfilled");
  llvm::BasicBlock *rejectedBlock = codegen_.functionFrame().newBlock("for.await.rejected");
  codegen_.builder_.CreateCondBr(codegen_.builder_.CreateICmpEQ(state, codegen_.builder_.getInt32(kPromiseFulfilled)),
                        fulfilledBlock, rejectedBlock,
                        llvm::MDBuilder(codegen_.ctx_).createLikelyBranchWeights());
  codegen_.functionFrame().switchTo(rejectedBlock);
  llvm::Value *error = codegen_.builder_.CreateLoad(ptr, codegen_.promiseLowerer().promiseField(step, kPromiseError), "error");
  codegen_.builder_.CreateCall(codegen_.objRetainFn_, {error});
  codegen_.builder_.CreateCall(codegen_.instanceReleaseFn_, {step});
  forget();
  codegen_.builder_.CreateStore(error, codegen_.throwLowerer().thrownSlot());
  codegen_.builder_.CreateCall(codegen_.processLowerer().processFunction("tz_error_frame", codegen_.builder_.getVoidTy(), {ptr}),
                      {codegen_.asyncLowerer().asyncFrameDescriptor(loc)});
  codegen_.builder_.CreateBr(codegen_.unwindLowerer().unwindTarget());
  codegen_.functionFrame().switchTo(fulfilledBlock);
  llvm::Value *flags = codegen_.builder_.CreateLoad(i32, codegen_.promiseLowerer().promiseField(step, kPromiseFlags), "flags");
  llvm::BasicBlock *doneBlock = codegen_.functionFrame().newBlock("for.await.done");
  llvm::BasicBlock *valueBlock = codegen_.functionFrame().newBlock("for.await.value");
  codegen_.builder_.CreateCondBr(
      codegen_.builder_.CreateICmpNE(codegen_.builder_.CreateAnd(flags, codegen_.builder_.getInt32(kPromiseDone)),
                            codegen_.builder_.getInt32(0)),
      doneBlock, valueBlock);
  codegen_.functionFrame().switchTo(doneBlock);
  codegen_.builder_.CreateCall(codegen_.instanceReleaseFn_, {step});
  forget();
  codegen_.builder_.CreateBr(endBlock);
  codegen_.functionFrame().switchTo(valueBlock);
  llvm::Value *value;
  if (codegen_.promiseLowerer().promiseValueSize(element)) {
    value = codegen_.builder_.CreateLoad(codegen_.moduleBuilder().lower(element), codegen_.promiseLowerer().promiseValueAddress(step), "value");
    codegen_.refCounts().retainValue(value, element);
  } else {
    value = element->isUndefined() ? static_cast<llvm::Value *>(codegen_.builder_.getFalse())
                                   : codegen_.optionalLowerer().undefinedValue(element);
  }
  codegen_.builder_.CreateCall(codegen_.instanceReleaseFn_, {step});

  return value;
}

void AsyncGeneratorLowerer::closeAsyncIterator(llvm::Value *slot) {
  llvm::Type *ptr = codegen_.builder_.getPtrTy();
  llvm::Value *it = codegen_.builder_.CreateLoad(ptr, slot, "iterator");
  llvm::BasicBlock *closeBlock = codegen_.functionFrame().newBlock("for.await.close");
  llvm::BasicBlock *nextBlock = codegen_.functionFrame().newBlock("for.await.closed");
  codegen_.builder_.CreateCondBr(codegen_.builder_.CreateIsNotNull(it), closeBlock, nextBlock);
  codegen_.functionFrame().switchTo(closeBlock);
  if (codegen_.cleanupNoAwait_ || !codegen_.async_) {
    // A coroutine dropped where it waits only lets go of what it holds.
    codegen_.builder_.CreateBr(nextBlock);
    codegen_.functionFrame().switchTo(nextBlock);
    return;
  }
  // The error in flight, if any, waits meanwhile; one that closing gives is
  // dropped, as a sync iterator's is (see iterCloseFunction).
  llvm::Value *saved = codegen_.builder_.CreateLoad(ptr, codegen_.throwLowerer().thrownSlot(), "saved");
  codegen_.builder_.CreateStore(llvm::ConstantPointerNull::get(ptr), codegen_.throwLowerer().thrownSlot());
  llvm::Value *closed = codegen_.promiseLowerer().newPromise(Type::voidTy());
  codegen_.builder_.CreateCall(codegen_.asyncLowerer().asyncFunction("tz_async_iter_close", codegen_.builder_.getVoidTy(), {ptr, ptr}),
                      {it, closed});
  codegen_.asyncLowerer().suspendAsync(closed, codegen_.loc_);
  codegen_.builder_.CreateCall(codegen_.instanceReleaseFn_, {closed});
  codegen_.builder_.CreateStore(saved, codegen_.throwLowerer().thrownSlot());
  codegen_.builder_.CreateBr(nextBlock);
  codegen_.functionFrame().switchTo(nextBlock);
}

// `yield* xs` of an async generator: each value of the async iterator xs
// as the generator's own, without awaiting it again; or each element of an
// array or an iterator awaited, as the wrapping iterator of the spec does,
// and then the turn of `yield*`'s own await of the step.
void AsyncGeneratorLowerer::emitAsyncYieldAll(YieldExpr *yield, const Type *yielded) {
  const Type *type = yield->value->type;
  const Type *element = yield->element;
  codegen_.functionFrame().pushScope();
  if (type->isAsyncIterator() || type->isRuntimeObject()) {
    llvm::Value *slot = codegen_.functionFrame().createManagedSlot(type, "yield.iterator");
    codegen_.builder_.CreateStore(codegen_.exprLowerer().emitExpr(yield->value), slot);
    codegen_.scopes_.back().asyncIterators.push_back(slot);
    llvm::BasicBlock *headBlock = codegen_.functionFrame().newBlock("yield.all.next");
    llvm::BasicBlock *endBlock = codegen_.functionFrame().newBlock("yield.all.end");
    codegen_.functionFrame().emitBlock(headBlock);
    llvm::Value *value = emitAsyncStep(slot, element, yield->loc(), endBlock);
    agenYield(codegen_.exprLowerer().emitConversion(value, element, yielded), yielded, yield->loc());
    codegen_.builder_.CreateBr(headBlock);
    codegen_.functionFrame().switchTo(endBlock);
    codegen_.functionFrame().popScope();
    return;
  }
  CodeGen::Producer producer = codegen_.inlineGeneratorLowerer().prepare(yield->value);
  size_t depth = codegen_.scopes_.size();
  llvm::BasicBlock *endBlock = codegen_.functionFrame().newBlock("yield.all.end");
  const Type *from = type->isArray() ? type->inner() : type->isString() ? type : type->yielded();
  llvm::SMLoc loc = yield->loc();
  CodeGen::Consumer consumer;
  consumer.stop = [this, depth, endBlock] {
    codegen_.functionFrame().emitCleanups(depth);
    codegen_.builder_.CreateBr(endBlock);
  };
  consumer.each = [this, from, element, yielded, loc](llvm::Value *value) -> llvm::Value * {
    llvm::Value *awaited = awaitValueOf(value, from, loc);
    codegen_.asyncLowerer().suspendAsync(nullptr, loc);
    agenYield(codegen_.exprLowerer().emitConversion(awaited, element, yielded), yielded, loc);

    return nullptr;
  };
  producer.run(consumer);
  codegen_.functionFrame().emitBlock(endBlock);
  codegen_.functionFrame().popScope();
}

void AsyncGeneratorLowerer::emitForAwait(ForOfStmt *stmt) {
  llvm::SmallVector<llvm::StringRef, 1> labels = std::move(codegen_.pendingLabels_);
  codegen_.pendingLabels_.clear();
  const Type *type = stmt->iterable->type;
  const Type *element = stmt->binding.sym->type;
  codegen_.functionFrame().pushScope(); // the iterable
  if (type->isAsyncIterator() || type->isRuntimeObject()) {
    llvm::Value *slot = codegen_.functionFrame().createManagedSlot(type, "for.await.iterator");
    codegen_.builder_.CreateStore(codegen_.exprLowerer().emitExpr(stmt->iterable), slot);
    codegen_.scopes_.back().asyncIterators.push_back(slot);
    size_t depth = codegen_.scopes_.size();
    llvm::BasicBlock *headBlock = codegen_.functionFrame().newBlock("for.await.next");
    llvm::BasicBlock *endBlock = codegen_.functionFrame().newBlock("for.await.end");
    codegen_.functionFrame().emitBlock(headBlock);
    llvm::Value *value = emitAsyncStep(slot, element, stmt->loc(), endBlock);
    codegen_.functionFrame().pushScope(); // the loop variable, fresh in every iteration
    codegen_.stmtLowerer().storeReplacing(codegen_.functionFrame().createLocal(stmt->binding.sym, stmt->binding.name), value, element);
    codegen_.targets_.push_back({labels, /*isLoop=*/true, /*isSwitch=*/false, endBlock, headBlock, depth});
    codegen_.stmtLowerer().emitStmt(stmt->body);
    codegen_.targets_.pop_back();
    codegen_.functionFrame().popScope();
    codegen_.functionFrame().branchTo(headBlock);
    codegen_.functionFrame().switchTo(endBlock);
    codegen_.functionFrame().popScope();
    return;
  }
  CodeGen::Producer producer = codegen_.inlineGeneratorLowerer().prepare(stmt->iterable);
  size_t depth = codegen_.scopes_.size();
  llvm::BasicBlock *endBlock = codegen_.functionFrame().newBlock("for.await.end");
  const Type *from = type->isArray() ? type->inner() : type->isString() ? type : type->yielded();
  CodeGen::Consumer consumer;
  consumer.stop = [this, depth, endBlock] {
    codegen_.functionFrame().emitCleanups(depth);
    codegen_.builder_.CreateBr(endBlock);
  };
  consumer.each = [this, stmt, labels, depth, endBlock, from,
                   element](llvm::Value *value) -> llvm::Value * {
    llvm::Value *awaited = awaitValueOf(value, from, stmt->loc());
    codegen_.asyncLowerer().suspendAsync(nullptr, stmt->loc()); // the turn of the wrapping iterator
    const Type *given = Sema::awaitedTypeOf(from);
    llvm::Value *converted = element->isUndefined() ? codegen_.builder_.getFalse()
                             : given->isVoid()      ? codegen_.optionalLowerer().undefinedValue(element)
                                                    : codegen_.exprLowerer().emitConversion(awaited, given, element);
    if (element->isUndefined() && awaited && !given->isVoid()) {
      codegen_.refCounts().releaseValue(awaited, given);
    }
    llvm::BasicBlock *nextBlock = codegen_.functionFrame().newBlock("for.await.next");
    size_t here = codegen_.scopes_.size();
    codegen_.functionFrame().pushScope(); // the loop variable, fresh in every iteration
    codegen_.stmtLowerer().storeReplacing(codegen_.functionFrame().createLocal(stmt->binding.sym, stmt->binding.name), converted, element);
    codegen_.targets_.push_back(
        {labels, /*isLoop=*/true, /*isSwitch=*/false, endBlock, nextBlock, depth, here});
    codegen_.stmtLowerer().emitStmt(stmt->body);
    codegen_.targets_.pop_back();
    codegen_.functionFrame().popScope();
    codegen_.functionFrame().emitBlock(nextBlock);

    return nullptr;
  };
  producer.run(consumer);
  codegen_.functionFrame().emitBlock(endBlock);
  codegen_.functionFrame().popScope();
}

} // namespace tz
