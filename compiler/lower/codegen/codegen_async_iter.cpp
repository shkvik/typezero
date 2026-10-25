// Async generators and `for await` (docs/iterators.md, step 4).
//
// An async generator is a coroutine of LLVM that both awaits, as an async
// function does, and yields, as a generator does. Its object is a TzAsyncGen
// of the runtime (runtime/core/tzrt/tzrt.h): { i64 rc, ptr layout, the queue of
// the steps asked of it, its state, the reactor it awaits with }, and then its
// frame, so that the runtime resumes it as it resumes an async function
// (tz.async.resume, which goes to the frame after the reactor). The runtime
// keeps the state machine of the spec (AsyncGeneratorEnqueue, Yield,
// CompleteStep, DrainQueue, in runtime/core/async/async.c); the code here gives
// each `yield` its value and asks what to do next: go on, wait for the next
// step, or return, which runs the `finally` blocks around it.
//
// `for await` asks an async iterator for each value through its layout
// (TzAsyncIterLayout): a promise of the value, or one fulfilled with the flag
// TZ_PROMISE_DONE when it is done, which the loop awaits. An iterator left
// early (`break`, `return`, an error) is closed, its return(), and the loop
// waits for that too, as AsyncIteratorClose does. Over an array or an
// iterator, each element is awaited, and then a turn more, as the iterator
// of the spec that wraps a sync one (CreateAsyncFromSyncIterator) takes.

#include "compiler/lower/codegen/codegen.h"
#include "compiler/check/sema/sema.h"

#include "llvm/IR/MDBuilder.h"

using llvm::cast;

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

// The layout of async generators: the drop function, which destroys the
// coroutine where it waits (without awaiting anything, as JavaScript drops
// one), and what `for await` calls.
llvm::Constant *CodeGen::asyncGeneratorLayout() {
  if (agenLayout_)
    return agenLayout_;
  llvm::Type *ptr = builder_.getPtrTy();
  auto *drop = llvm::Function::Create(llvm::FunctionType::get(builder_.getVoidTy(), {ptr}, false),
                                      llvm::GlobalValue::InternalLinkage, "tz.agen.destroy",
                                      module_.get());
  drop->addFnAttr(llvm::Attribute::NoUnwind);
  {
    llvm::IRBuilder<> b(llvm::BasicBlock::Create(ctx_, "entry", drop));
    b.CreateCall(asyncFunction("tz_agen_drop_steps", b.getVoidTy(), {ptr}), {drop->getArg(0)});
    llvm::Value *frame =
        b.CreateConstInBoundsGEP1_64(b.getInt8Ty(), drop->getArg(0), kAgenHeader, "frame");
    b.CreateCall(coroIntrinsic(llvm::Intrinsic::coro_destroy), {frame});
    b.CreateRetVoid();
  }
  auto callee = [&](llvm::StringRef name) {
    return cast<llvm::Constant>(
        asyncFunction(name, builder_.getVoidTy(), {ptr, ptr}).getCallee());
  };
  llvm::Constant *null = llvm::ConstantPointerNull::get(builder_.getPtrTy());
  auto *type = llvm::StructType::get(ctx_, {layoutDescTy_, ptr, ptr});
  llvm::Constant *base = llvm::ConstantStruct::get(
      layoutDescTy_, {drop, null, builder_.getInt32(0), builder_.getInt32(kLayoutAsyncGenerator),
                      null, builder_.getInt32(0), builder_.getInt32(0)});
  auto *global = new llvm::GlobalVariable(
      *module_, type, /*isConstant=*/true, llvm::GlobalValue::PrivateLinkage,
      llvm::ConstantStruct::get(type, {base, callee("tz_agen_next"), callee("tz_agen_close")}),
      "tz.layout.agen");
  global->setUnnamedAddr(llvm::GlobalValue::UnnamedAddr::Global);
  agenLayout_ = global;
  return agenLayout_;
}

// An async generator function: a call binds the parameters and returns the
// generator, whose body starts with the first step asked of it.
void CodeGen::emitAsyncGenerator(FunctionDecl *fn, llvm::Function *function) {
  beginFunction(function, fn);
  Coroutine coroutine{};
  coroutine.header = kAgenHeader;
  coro_ = &coroutine;
  AsyncState state{};
  state.generator = true;
  async_ = &state;
  function_->addFnAttr(llvm::Attribute::PresplitCoroutine);
  llvm::Type *i8 = builder_.getInt8Ty(), *i64 = builder_.getInt64Ty();
  llvm::Value *null = llvm::ConstantPointerNull::get(builder_.getPtrTy());
  coroutine.id = builder_.CreateCall(coroIntrinsic(llvm::Intrinsic::coro_id),
                                     {builder_.getInt32(kAgenFrameAlign), null, null, null}, "id");
  llvm::Value *size =
      builder_.CreateCall(coroIntrinsic(llvm::Intrinsic::coro_size, {i64}), {}, "size");
  llvm::Value *memory = builder_.CreateCall(
      rt_.objAlloc, {builder_.CreateAdd(size, builder_.getInt64(kAgenHeader))}, "memory");
  builder_.CreateMemSet(memory, builder_.getInt8(0), kAgenHeader, llvm::MaybeAlign(8));
  builder_.CreateStore(builder_.getInt64(1), memory);
  builder_.CreateStore(asyncGeneratorLayout(), builder_.CreateConstInBoundsGEP1_64(i8, memory, 8));
  llvm::Value *reactor = builder_.CreateConstInBoundsGEP1_64(i8, memory, kAgenReactor);
  builder_.CreateStore(asyncResumeFunction(), builder_.CreateConstInBoundsGEP1_64(i8, reactor, 8));
  builder_.CreateStore(memory, builder_.CreateConstInBoundsGEP1_64(i8, reactor, 16));
  llvm::Value *frame = builder_.CreateConstInBoundsGEP1_64(i8, memory, kAgenHeader, "frame");
  coroutine.handle = builder_.CreateCall(coroIntrinsic(llvm::Intrinsic::coro_begin),
                                         {coroutine.id, frame}, "handle");
  // As for a generator, the object comes from the handle: LLVM takes the
  // frame to be reached only through it.
  coroutine.object = builder_.CreateGEP(
      i8, coroutine.handle, llvm::ConstantInt::getSigned(i64, -int64_t(kAgenHeader)), "generator");
  state.object = coroutine.object;
  state.reactor =
      builder_.CreateConstInBoundsGEP1_64(i8, coroutine.object, kAgenReactor, "reactor");
  coroutine.finalBlock = newBlock("coro.final");
  coroutine.freeBlock = newBlock("coro.free");
  coroutine.returnBlock = newBlock("coro.return");

  pushScope(); // the parameters, and the environment of a closure
  std::vector<llvm::Value *> args;
  unsigned first = 0;
  if (fn->isClosure() || fn->takesThis()) {
    envParam_ = function_->getArg(0);
    if (fn->isClosure())
      bindCaptures(fn);
    else
      envParam_->setName("this");
    llvm::Value *slot = createObjectSlot("env");
    builder_.CreateCall(objRetainFn_, {envParam_});
    builder_.CreateStore(envParam_, slot);
    first = 1;
  }
  for (unsigned i = first; i < function_->arg_size(); ++i)
    args.push_back(function_->getArg(i));
  emitParams(fn, args);
  emitSuspend(); // until the first step
  emitStmt(fn->body);
  if (blockIsOpen())
    emitCoroutineReturn(nullptr, nullptr);
  finishScopeUnwind(0);
  scopes_.pop_back(); // every way out released it
  finishFunctionUnwind();
  finishCoroutine();
  coro_ = nullptr;
  async_ = nullptr;
  function_ = nullptr;
}

// The body is done: the step it ran for gets done, then those waiting.
void CodeGen::finishAsyncGenerator(llvm::Value *error) {
  llvm::Type *ptr = builder_.getPtrTy();
  builder_.CreateCall(asyncFunction("tz_agen_finish", builder_.getVoidTy(), {ptr, ptr}),
                      {async_->object, error ? error : llvm::ConstantPointerNull::get(ptr)});
}

void CodeGen::emitAsyncGeneratorReturn(ReturnStmt *stmt) {
  if (stmt->value) {
    const Type *type = stmt->value->type;
    llvm::Value *value = awaitValueOf(emitExpr(stmt->value), type, stmt->loc());
    const Type *given = Sema::awaitedTypeOf(type);
    if (value && !given->isVoid())
      releaseValue(value, given);
  }
  emitReturnOf(nullptr);
}

llvm::Value *CodeGen::awaitValueOf(llvm::Value *value, const Type *type, llvm::SMLoc loc) {
  if (type->isPromise())
    return awaitPromise(value, type, loc);
  if (holdsPromise(type))
    return awaitUnion(value, type, Sema::awaitedTypeOf(type), loc);
  suspendAsync(nullptr, loc);
  return value;
}

// AsyncGeneratorYield: `value` (owned, of `type`, null for none) settles
// the step that runs, and the body goes on with what comes next: the next
// step at once, or later; or return() returns from here, after a turn.
void CodeGen::agenYield(llvm::Value *value, const Type *type, llvm::SMLoc loc) {
  llvm::Type *ptr = builder_.getPtrTy(), *i32 = builder_.getInt32Ty();
  llvm::Value *slot = value && promiseValueSize(type)
                          ? spill(value, "yielded")
                          : static_cast<llvm::Value *>(llvm::ConstantPointerNull::get(ptr));
  if (value && !promiseValueSize(type))
    releaseValue(value, type);
  llvm::Value *descriptor = promiseValueSize(type)
                                ? static_cast<llvm::Value *>(typeDescriptor(type))
                                : llvm::ConstantPointerNull::get(ptr);
  llvm::Value *next = builder_.CreateCall(asyncFunction("tz_agen_yield", i32, {ptr, ptr, ptr}),
                                          {async_->object, slot, descriptor}, "next");
  llvm::BasicBlock *waitBlock = newBlock("yield.wait");
  llvm::BasicBlock *decideBlock = newBlock("yield.decide");
  llvm::BasicBlock *closeBlock = newBlock("yield.close");
  llvm::BasicBlock *goBlock = newBlock("yield.go");
  llvm::BasicBlock *before = builder_.GetInsertBlock();
  builder_.CreateCondBr(builder_.CreateICmpSLT(next, builder_.getInt32(0)), waitBlock,
                        decideBlock);
  switchTo(waitBlock);
  emitSuspend();
  llvm::Value *resumed = builder_.CreateCall(asyncFunction("tz_agen_resumed", i32, {ptr}),
                                             {async_->object}, "resumed");
  llvm::BasicBlock *waited = builder_.GetInsertBlock();
  builder_.CreateBr(decideBlock);
  switchTo(decideBlock);
  llvm::PHINode *how = builder_.CreatePHI(i32, 2, "how");
  how->addIncoming(next, before);
  how->addIncoming(resumed, waited);
  builder_.CreateCondBr(builder_.CreateICmpEQ(how, builder_.getInt32(1)), closeBlock, goBlock,
                        llvm::MDBuilder(ctx_).createUnlikelyBranchWeights());
  // return(): AsyncGeneratorUnwrapYieldResumption awaits the value it
  // returns, then returns from here.
  switchTo(closeBlock);
  suspendAsync(nullptr, loc);
  emitReturnOf(nullptr);
  if (blockIsOpen())
    builder_.CreateUnreachable();
  switchTo(goBlock);
}

llvm::Value *CodeGen::emitAsyncYield(YieldExpr *yield) {
  const Type *yielded = currentFn_->sym->ret->yielded();
  if (yield->delegate) {
    emitAsyncYieldAll(yield, yielded);
    return builder_.getFalse(); // undefined
  }
  // `yield value` awaits the value first.
  llvm::Value *value = nullptr;
  const Type *given = Type::undefinedTy();
  if (yield->value) {
    const Type *type = yield->value->type;
    value = awaitValueOf(emitExpr(yield->value), type, yield->loc());
    given = Sema::awaitedTypeOf(type);
  } else {
    suspendAsync(nullptr, yield->loc());
  }
  llvm::Value *converted = nullptr;
  if (value && !given->isVoid() && !given->isUndefined())
    converted = emitConversion(value, given, yielded);
  else if (!yielded->isUndefined() && yielded->isOptional())
    converted = undefinedValue(yielded);
  agenYield(converted, yielded, yield->loc());
  return builder_.getFalse(); // what next() sent: undefined
}

// One step of the async iterator held in `slot`, of type `type`: the value
// it gives (owned, of `element`), or on to `endBlock` once it is done. The
// iterator that is done, or whose step was rejected, is not closed.
llvm::Value *CodeGen::emitAsyncStep(llvm::Value *slot, const Type *element, llvm::SMLoc loc,
                                    llvm::BasicBlock *endBlock) {
  llvm::Type *ptr = builder_.getPtrTy(), *i32 = builder_.getInt32Ty();
  llvm::Value *it = builder_.CreateLoad(ptr, slot, "iterator");
  llvm::Value *step = newPromise(element);
  builder_.CreateCall(asyncFunction("tz_async_iter_next", builder_.getVoidTy(), {ptr, ptr}),
                      {it, step});
  suspendAsync(step, loc);
  // Let go of the iterator without closing it.
  auto forget = [&] {
    llvm::Value *held = builder_.CreateLoad(ptr, slot, "iterator");
    builder_.CreateStore(llvm::ConstantPointerNull::get(ptr), slot);
    builder_.CreateCall(instanceReleaseFn_, {held});
  };
  llvm::Value *state = builder_.CreateLoad(i32, promiseField(step, kPromiseState), "state");
  llvm::BasicBlock *fulfilledBlock = newBlock("for.await.fulfilled");
  llvm::BasicBlock *rejectedBlock = newBlock("for.await.rejected");
  builder_.CreateCondBr(builder_.CreateICmpEQ(state, builder_.getInt32(kPromiseFulfilled)),
                        fulfilledBlock, rejectedBlock,
                        llvm::MDBuilder(ctx_).createLikelyBranchWeights());
  switchTo(rejectedBlock);
  llvm::Value *error = builder_.CreateLoad(ptr, promiseField(step, kPromiseError), "error");
  builder_.CreateCall(objRetainFn_, {error});
  builder_.CreateCall(instanceReleaseFn_, {step});
  forget();
  builder_.CreateStore(error, thrownSlot());
  builder_.CreateCall(processFunction("tz_error_frame", builder_.getVoidTy(), {ptr}),
                      {asyncFrameDescriptor(loc)});
  builder_.CreateBr(unwindTarget());
  switchTo(fulfilledBlock);
  llvm::Value *flags = builder_.CreateLoad(i32, promiseField(step, kPromiseFlags), "flags");
  llvm::BasicBlock *doneBlock = newBlock("for.await.done");
  llvm::BasicBlock *valueBlock = newBlock("for.await.value");
  builder_.CreateCondBr(
      builder_.CreateICmpNE(builder_.CreateAnd(flags, builder_.getInt32(kPromiseDone)),
                            builder_.getInt32(0)),
      doneBlock, valueBlock);
  switchTo(doneBlock);
  builder_.CreateCall(instanceReleaseFn_, {step});
  forget();
  builder_.CreateBr(endBlock);
  switchTo(valueBlock);
  llvm::Value *value;
  if (promiseValueSize(element)) {
    value = builder_.CreateLoad(lower(element), promiseValueAddress(step), "value");
    retainValue(value, element);
  } else {
    value = element->isUndefined() ? static_cast<llvm::Value *>(builder_.getFalse())
                                   : undefinedValue(element);
  }
  builder_.CreateCall(instanceReleaseFn_, {step});
  return value;
}

void CodeGen::closeAsyncIterator(llvm::Value *slot) {
  llvm::Type *ptr = builder_.getPtrTy();
  llvm::Value *it = builder_.CreateLoad(ptr, slot, "iterator");
  llvm::BasicBlock *closeBlock = newBlock("for.await.close");
  llvm::BasicBlock *nextBlock = newBlock("for.await.closed");
  builder_.CreateCondBr(builder_.CreateIsNotNull(it), closeBlock, nextBlock);
  switchTo(closeBlock);
  if (cleanupNoAwait_ || !async_) {
    // A coroutine dropped where it waits only lets go of what it holds.
    builder_.CreateBr(nextBlock);
    switchTo(nextBlock);
    return;
  }
  // The error in flight, if any, waits meanwhile; one that closing gives is
  // dropped, as a sync iterator's is (see iterCloseFunction).
  llvm::Value *saved = builder_.CreateLoad(ptr, thrownSlot(), "saved");
  builder_.CreateStore(llvm::ConstantPointerNull::get(ptr), thrownSlot());
  llvm::Value *closed = newPromise(Type::voidTy());
  builder_.CreateCall(asyncFunction("tz_async_iter_close", builder_.getVoidTy(), {ptr, ptr}),
                      {it, closed});
  suspendAsync(closed, loc_);
  builder_.CreateCall(instanceReleaseFn_, {closed});
  builder_.CreateStore(saved, thrownSlot());
  builder_.CreateBr(nextBlock);
  switchTo(nextBlock);
}

// `yield* xs` of an async generator: each value of the async iterator xs
// as the generator's own, without awaiting it again; or each element of an
// array or an iterator awaited, as the wrapping iterator of the spec does,
// and then the turn of `yield*`'s own await of the step.
void CodeGen::emitAsyncYieldAll(YieldExpr *yield, const Type *yielded) {
  const Type *type = yield->value->type;
  const Type *element = yield->element;
  pushScope();
  if (type->isAsyncIterator() || type->isRuntimeObject()) {
    llvm::Value *slot = createManagedSlot(type, "yield.iterator");
    builder_.CreateStore(emitExpr(yield->value), slot);
    scopes_.back().asyncIterators.push_back(slot);
    llvm::BasicBlock *headBlock = newBlock("yield.all.next");
    llvm::BasicBlock *endBlock = newBlock("yield.all.end");
    emitBlock(headBlock);
    llvm::Value *value = emitAsyncStep(slot, element, yield->loc(), endBlock);
    agenYield(emitConversion(value, element, yielded), yielded, yield->loc());
    builder_.CreateBr(headBlock);
    switchTo(endBlock);
    popScope();
    return;
  }
  Producer producer = prepare(yield->value);
  size_t depth = scopes_.size();
  llvm::BasicBlock *endBlock = newBlock("yield.all.end");
  const Type *from = type->isArray() ? type->inner() : type->isString() ? type : type->yielded();
  llvm::SMLoc loc = yield->loc();
  Consumer consumer;
  consumer.stop = [this, depth, endBlock] {
    emitCleanups(depth);
    builder_.CreateBr(endBlock);
  };
  consumer.each = [this, from, element, yielded, loc](llvm::Value *value) -> llvm::Value * {
    llvm::Value *awaited = awaitValueOf(value, from, loc);
    suspendAsync(nullptr, loc);
    agenYield(emitConversion(awaited, element, yielded), yielded, loc);
    return nullptr;
  };
  producer.run(consumer);
  emitBlock(endBlock);
  popScope();
}

void CodeGen::emitForAwait(ForOfStmt *stmt) {
  llvm::SmallVector<llvm::StringRef, 1> labels = std::move(pendingLabels_);
  pendingLabels_.clear();
  const Type *type = stmt->iterable->type;
  const Type *element = stmt->binding.sym->type;
  pushScope(); // the iterable
  if (type->isAsyncIterator() || type->isRuntimeObject()) {
    llvm::Value *slot = createManagedSlot(type, "for.await.iterator");
    builder_.CreateStore(emitExpr(stmt->iterable), slot);
    scopes_.back().asyncIterators.push_back(slot);
    size_t depth = scopes_.size();
    llvm::BasicBlock *headBlock = newBlock("for.await.next");
    llvm::BasicBlock *endBlock = newBlock("for.await.end");
    emitBlock(headBlock);
    llvm::Value *value = emitAsyncStep(slot, element, stmt->loc(), endBlock);
    pushScope(); // the loop variable, fresh in every iteration
    storeReplacing(createLocal(stmt->binding.sym, stmt->binding.name), value, element);
    targets_.push_back({labels, /*isLoop=*/true, /*isSwitch=*/false, endBlock, headBlock, depth});
    emitStmt(stmt->body);
    targets_.pop_back();
    popScope();
    branchTo(headBlock);
    switchTo(endBlock);
    popScope();
    return;
  }
  Producer producer = prepare(stmt->iterable);
  size_t depth = scopes_.size();
  llvm::BasicBlock *endBlock = newBlock("for.await.end");
  const Type *from = type->isArray() ? type->inner() : type->isString() ? type : type->yielded();
  Consumer consumer;
  consumer.stop = [this, depth, endBlock] {
    emitCleanups(depth);
    builder_.CreateBr(endBlock);
  };
  consumer.each = [this, stmt, labels, depth, endBlock, from,
                   element](llvm::Value *value) -> llvm::Value * {
    llvm::Value *awaited = awaitValueOf(value, from, stmt->loc());
    suspendAsync(nullptr, stmt->loc()); // the turn of the wrapping iterator
    const Type *given = Sema::awaitedTypeOf(from);
    llvm::Value *converted = element->isUndefined() ? builder_.getFalse()
                             : given->isVoid()      ? undefinedValue(element)
                                                    : emitConversion(awaited, given, element);
    if (element->isUndefined() && awaited && !given->isVoid())
      releaseValue(awaited, given);
    llvm::BasicBlock *nextBlock = newBlock("for.await.next");
    size_t here = scopes_.size();
    pushScope(); // the loop variable, fresh in every iteration
    storeReplacing(createLocal(stmt->binding.sym, stmt->binding.name), converted, element);
    targets_.push_back(
        {labels, /*isLoop=*/true, /*isSwitch=*/false, endBlock, nextBlock, depth, here});
    emitStmt(stmt->body);
    targets_.pop_back();
    popScope();
    emitBlock(nextBlock);
    return nullptr;
  };
  producer.run(consumer);
  emitBlock(endBlock);
  popScope();
}

} // namespace tz
