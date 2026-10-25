// Async (docs/async.md, step 1): async functions as coroutines, `await`,
// Promises and their methods, timers and the queues.
//
// A Promise is an object of the runtime (TzPromise in runtime/tzrt.h):
// { i64 rc, ptr layout, ptr head, ptr tail, ptr error, i32 state, i32 flags }
// and then its value, at TZ_PROMISE_VALUE. Its layout says the type of the
// value, for the runtime to release, copy and print it; the code stores and
// reads the value itself, and the runtime settles the promise and queues its
// reactions (runtime/async.c).
//
// An async function is a coroutine of LLVM (the switch-resumed kind, as a
// generator is) whose frame lies in the same allocation as its promise:
// [promise][reactor][frame]. A call runs the body up to the first `await`,
// as in JavaScript, and gives the promise. `await p` hands the reactor to p,
// which queues it once p settles; the runtime then resumes the coroutine
// (tz.async.resume), which reads p's value or throws its error. The
// coroutine holds its own object while it waits, so a promise nobody keeps
// still finishes; it releases it when the runtime has resumed it. `await x`
// of a value that is no Promise just queues the reactor: one turn of the
// queue, as in JavaScript, and nothing is allocated.
//
// then(), catch() and finally() make a reaction object with the callbacks
// and the promise they give, whose code (reactionHandler) the compiler makes
// for the types at hand. Top-level code with `await` is a coroutine itself,
// tz.main.async, whose promise main hands to the runtime.

#include "codegen/codegen.h"
#include "sema/sema.h"

#include "llvm/ADT/STLExtras.h"
#include "llvm/IR/MDBuilder.h"

using llvm::cast;
using llvm::dyn_cast;
using llvm::isa;

namespace tz {

namespace {

// Must match runtime/tzrt.h.
constexpr uint64_t kPromiseHeader = 48; // TZ_PROMISE_VALUE
constexpr uint64_t kReactorSize = 32;
enum PromiseField : unsigned { kRc, kLayout, kHead, kTail, kError, kState, kFlags };
constexpr uint32_t kFulfilled = 1;
constexpr uint32_t kLayoutPromise = 131072; // TZ_LAYOUT_PROMISE
enum CombineKind : int32_t { kAll, kAllSettled, kRace, kAny };
// The alignment of a coroutine's frame, as for generators.
constexpr uint32_t kAsyncFrameAlign = 8;

// A reaction to a promise (then, catch, finally): { i64 rc, ptr descriptor,
// reactor { ptr next, ptr run, ptr owner, ptr source }, ptr derived, fn
// onFulfilled, fn onRejected }.
enum ReactionField : unsigned {
  kReactionRc,
  kReactionDesc,
  kReactorNext,
  kReactorRun,
  kReactorOwner,
  kReactorSource,
  kReactionDerived,
  kReactionFulfilled,
  kReactionRejected,
};
constexpr uint64_t kReactorOffset = 16;

// `Promise.resolve(x)` of a value that is no Promise.
const Expr *resolvedValue(const Expr *expr) {
  auto *call = dyn_cast<CallExpr>(expr);
  if (!call || call->builtin != Builtin::PromiseResolve || call->args.size() != 1 ||
      call->args[0]->type->isPromise())
    return nullptr;
  return call->args[0];
}

} // namespace

//===----------------------------------------------------------------------===//
// Promises
//===----------------------------------------------------------------------===//

llvm::FunctionCallee CodeGen::asyncFunction(llvm::StringRef name, llvm::Type *ret,
                                            llvm::ArrayRef<llvm::Type *> params) {
  return processFunction(name, ret, params);
}

uint64_t CodeGen::promiseValueSize(const Type *value) {
  if (value->isVoid() || value->isNever() || value->isUndefined())
    return 0;
  uint64_t size = module_->getDataLayout().getTypeAllocSize(lower(value));
  return (size + 7) / 8 * 8;
}

llvm::Value *CodeGen::promiseValueAddress(llvm::Value *promise) {
  return builder_.CreateConstInBoundsGEP1_64(builder_.getInt8Ty(), promise, kPromiseHeader,
                                             "promise.value");
}

llvm::Constant *CodeGen::promiseLayout(const Type *value) {
  llvm::Constant *&layout = promiseLayouts_[value];
  if (layout)
    return layout;
  llvm::Type *ptr = builder_.getPtrTy();
  llvm::Constant *fields = llvm::ConstantPointerNull::get(builder_.getPtrTy());
  uint32_t count = 0;
  if (promiseValueSize(value)) {
    auto *field = new llvm::GlobalVariable(
        *module_, fieldDescTy_, /*isConstant=*/true, llvm::GlobalValue::PrivateLinkage,
        llvm::ConstantStruct::get(fieldDescTy_, {utf8Constant(""), builder_.getInt32(0),
                                                 builder_.getInt32(uint32_t(kPromiseHeader)),
                                                 typeDescriptor(value)}),
        "tz.promise.field");
    field->setUnnamedAddr(llvm::GlobalValue::UnnamedAddr::Global);
    fields = field;
    count = 1;
  }
  auto *drop = cast<llvm::Function>(
      asyncFunction("tz_promise_drop", builder_.getVoidTy(), {ptr}).getCallee());
  // TZ_LAYOUT_GROUP: its promises are members of groups (runtime/async.c).
  uint32_t flags = kLayoutPromise | (promiseIsMember(value) ? 0x80000000u : 0u);
  auto *global = new llvm::GlobalVariable(
      *module_, layoutDescTy_, /*isConstant=*/true, llvm::GlobalValue::PrivateLinkage,
      llvm::ConstantStruct::get(layoutDescTy_, {drop, fields, builder_.getInt32(count),
                                                builder_.getInt32(flags),
                                                llvm::ConstantPointerNull::get(builder_.getPtrTy()),
                                                builder_.getInt32(0),
                                                builder_.getInt32(plainTypeId(Type::promise(value)))}),
      "tz.layout.promise");
  global->setUnnamedAddr(llvm::GlobalValue::UnnamedAddr::Global);
  layout = global;
  return layout;
}

llvm::Value *CodeGen::newPromise(const Type *value) {
  llvm::Type *ptr = builder_.getPtrTy();
  auto *fn = cast<llvm::Function>(asyncFunction("tz_promise_new", ptr, {ptr}).getCallee());
  fn->addRetAttr(llvm::Attribute::NoAlias);
  return builder_.CreateCall(fn, {promiseLayout(value)}, "promise");
}

void CodeGen::fulfillPromise(llvm::Value *promise, llvm::Value *value, const Type *type) {
  if (value && promiseValueSize(type))
    builder_.CreateStore(value, promiseValueAddress(promise));
  builder_.CreateCall(
      asyncFunction("tz_promise_fulfill", builder_.getVoidTy(), {builder_.getPtrTy()}), {promise});
}

void CodeGen::rejectPromise(llvm::Value *promise, llvm::Value *error) {
  llvm::Type *ptr = builder_.getPtrTy();
  builder_.CreateCall(asyncFunction("tz_promise_reject", builder_.getVoidTy(), {ptr, ptr}),
                      {promise, error});
}

void CodeGen::followPromise(llvm::Value *promise, llvm::Value *other) {
  llvm::Type *ptr = builder_.getPtrTy();
  builder_.CreateCall(asyncFunction("tz_promise_follow", builder_.getVoidTy(), {ptr, ptr}),
                      {promise, other});
}

llvm::Value *CodeGen::promiseField(llvm::Value *promise, unsigned field) {
  return builder_.CreateStructGEP(promiseObjTy_, promise, field);
}

//===----------------------------------------------------------------------===//
// Async functions
//===----------------------------------------------------------------------===//

// void (ptr reactor, ptr source): what the runtime calls to go on with an
// async function, whose frame follows the reactor.
llvm::Function *CodeGen::asyncResumeFunction() {
  if (asyncResumeFn_)
    return asyncResumeFn_;
  llvm::Type *ptr = builder_.getPtrTy();
  asyncResumeFn_ =
      llvm::Function::Create(llvm::FunctionType::get(builder_.getVoidTy(), {ptr, ptr}, false),
                             llvm::GlobalValue::InternalLinkage, "tz.async.resume", module_.get());
  asyncResumeFn_->addFnAttr(llvm::Attribute::NoUnwind);
  llvm::IRBuilder<> b(llvm::BasicBlock::Create(ctx_, "entry", asyncResumeFn_));
  llvm::Value *frame =
      b.CreateConstInBoundsGEP1_64(b.getInt8Ty(), asyncResumeFn_->getArg(0), kReactorSize, "frame");
  b.CreateCall(coroIntrinsic(llvm::Intrinsic::coro_resume), {frame});
  b.CreateRetVoid();
  return asyncResumeFn_;
}

// The start of an async function: the promise, the reactor and the frame
// in one allocation, which the ramp gives back.
void CodeGen::beginAsync() {
  function_->addFnAttr(llvm::Attribute::PresplitCoroutine);
  Coroutine &c = *coro_;
  AsyncState &a = *async_;
  llvm::Type *i8 = builder_.getInt8Ty(), *i64 = builder_.getInt64Ty();
  llvm::Value *null = llvm::ConstantPointerNull::get(builder_.getPtrTy());
  uint64_t reactorOffset = kPromiseHeader + promiseValueSize(a.value);
  uint64_t frameOffset = reactorOffset + kReactorSize;
  c.id = builder_.CreateCall(coroIntrinsic(llvm::Intrinsic::coro_id),
                             {builder_.getInt32(kAsyncFrameAlign), null, null, null}, "id");
  llvm::Value *size =
      builder_.CreateCall(coroIntrinsic(llvm::Intrinsic::coro_size, {i64}), {}, "size");
  // A promise that may hold a member is one, with its frame (see
  // promiseIsMember); its count is set already.
  bool member = promiseIsMember(a.value);
  llvm::Value *total = builder_.CreateAdd(size, builder_.getInt64(frameOffset));
  llvm::Value *memory =
      member ? builder_.CreateCall(groupFunction("tz_group_alloc", builder_.getPtrTy(), {i64}),
                                   {total}, "memory")
             : builder_.CreateCall(rt_.objAlloc, {total}, "memory");
  builder_.CreateMemSet(builder_.CreateConstInBoundsGEP1_64(i8, memory, 8), builder_.getInt8(0),
                        frameOffset - 8, llvm::MaybeAlign(8));
  if (!member)
    builder_.CreateStore(builder_.getInt64(1), memory);
  builder_.CreateStore(promiseLayout(a.value), builder_.CreateConstInBoundsGEP1_64(i8, memory, 8));
  llvm::Value *reactor = builder_.CreateConstInBoundsGEP1_64(i8, memory, reactorOffset);
  builder_.CreateStore(asyncResumeFunction(), builder_.CreateConstInBoundsGEP1_64(i8, reactor, 8));
  builder_.CreateStore(memory, builder_.CreateConstInBoundsGEP1_64(i8, reactor, 16));
  llvm::Value *frame = builder_.CreateConstInBoundsGEP1_64(i8, memory, frameOffset, "frame");
  c.handle =
      builder_.CreateCall(coroIntrinsic(llvm::Intrinsic::coro_begin), {c.id, frame}, "handle");
  // As for a generator, the object comes from the handle: LLVM takes the
  // frame to be reached only through it.
  c.object = builder_.CreateGEP(
      i8, c.handle, llvm::ConstantInt::getSigned(i64, -int64_t(frameOffset)), "promise");
  a.object = c.object;
  a.reactor = builder_.CreateConstInBoundsGEP1_64(i8, c.object, reactorOffset, "reactor");
  c.finalBlock = newBlock("coro.final");
  c.freeBlock = newBlock("coro.free");
  c.returnBlock = newBlock("coro.return");
}

// The final suspension. The memory is the promise's, which its last release
// frees: nothing destroys the coroutine.
void CodeGen::finishAsync() {
  Coroutine &c = *coro_;
  if (blockIsOpen())
    builder_.CreateUnreachable();
  llvm::Value *none = llvm::ConstantTokenNone::get(ctx_);
  switchTo(c.finalBlock);
  llvm::Value *state = builder_.CreateCall(coroIntrinsic(llvm::Intrinsic::coro_suspend),
                                           {none, builder_.getTrue()}, "final");
  llvm::BasicBlock *resumedBlock = newBlock("coro.resumed.done");
  llvm::SwitchInst *next = builder_.CreateSwitch(state, c.returnBlock, 2);
  next->addCase(builder_.getInt8(0), resumedBlock);
  next->addCase(builder_.getInt8(1), c.freeBlock);
  switchTo(resumedBlock);
  builder_.CreateUnreachable();
  switchTo(c.freeBlock);
  builder_.CreateBr(c.returnBlock);
  switchTo(c.returnBlock);
  builder_.CreateCall(coroIntrinsic(llvm::Intrinsic::coro_end),
                      {c.handle, builder_.getFalse(), none});
  // Only the ramp goes on after coro.end: it gives the promise to the
  // caller, or drops it for a callback whose promise nobody takes.
  if (async_->dropsPromise) {
    builder_.CreateCall(instanceReleaseFn_, {c.object});
    builder_.CreateRetVoid();
  } else {
    builder_.CreateRet(c.object);
  }
}

void CodeGen::emitAsyncFunction(FunctionDecl *fn, llvm::Function *function) {
  beginFunction(function, fn);
  Coroutine coroutine{};
  coro_ = &coroutine;
  AsyncState state{};
  async_ = &state;
  const Type *ret = fn->sym->ret;
  state.value = ret->isPromise() ? ret->promised() : Type::voidTy();
  state.dropsPromise = !ret->isPromise();
  beginAsync();
  pushScope(); // the parameters, and the environment of a closure
  std::vector<llvm::Value *> args;
  unsigned first = 0;
  if (fn->isClosure() || fn->takesThis()) {
    envParam_ = function_->getArg(0);
    if (fn->isClosure())
      bindCaptures(fn);
    else
      envParam_->setName("this");
    // The function value and the object of a method may go before the
    // coroutine is done: it keeps them.
    llvm::Value *slot = createObjectSlot("env");
    builder_.CreateCall(objRetainFn_, {envParam_});
    builder_.CreateStore(envParam_, slot);
    first = 1;
  }
  for (unsigned i = first; i < function_->arg_size(); ++i)
    args.push_back(function_->getArg(i));
  emitParams(fn, args);
  if (recursive_.contains(fn))
    emitStackCheck(fn->loc());
  emitStmt(fn->body);
  if (blockIsOpen())
    emitAsyncReturn(nullptr);
  finishScopeUnwind(0);
  scopes_.pop_back(); // every way out released it
  finishFunctionUnwind();
  finishAsync();
  coro_ = nullptr;
  async_ = nullptr;
  function_ = nullptr;
}

// The top-level code of a program that awaits: a coroutine of its own,
// whose promise is that of the code.
llvm::Function *CodeGen::emitAsyncMain(SourceFile &file) {
  auto *body =
      llvm::Function::Create(llvm::FunctionType::get(builder_.getPtrTy(), false),
                             llvm::GlobalValue::InternalLinkage, "tz.main.async", module_.get());
  setFunctionAttributes(body);
  beginFunction(body, nullptr);
  Coroutine coroutine{};
  coro_ = &coroutine;
  AsyncState state{};
  async_ = &state;
  state.value = Type::voidTy();
  beginAsync();
  pushScope();
  for (Stmt *item : file.items)
    if (!isa<FunctionDecl>(item))
      emitStmt(item);
  if (blockIsOpen())
    emitAsyncReturn(nullptr);
  finishScopeUnwind(0);
  scopes_.pop_back();
  finishFunctionUnwind();
  finishAsync();
  coro_ = nullptr;
  async_ = nullptr;
  function_ = nullptr;
  return body;
}

void CodeGen::emitAsyncReturn(llvm::Value *value) {
  AsyncState &a = *async_;
  llvm::Value *object = a.object;
  if (value && !promiseValueSize(a.value))
    value = nullptr;
  if (value)
    builder_.CreateStore(value, promiseValueAddress(object));
  emitCleanups(0);
  fulfillPromise(object, nullptr, a.value);
  builder_.CreateBr(coro_->finalBlock);
  startDeadBlock();
}

void CodeGen::emitAsyncUnwind() {
  llvm::Value *error = builder_.CreateLoad(builder_.getPtrTy(), thrownGlobal(), "error");
  builder_.CreateStore(llvm::ConstantPointerNull::get(builder_.getPtrTy()), thrownGlobal());
  if (async_->generator) // the step it ran for is rejected
    finishAsyncGenerator(error);
  else
    rejectPromise(async_->object, error);
  builder_.CreateBr(coro_->finalBlock);
}

void CodeGen::emitAsyncReturnStmt(ReturnStmt *stmt) {
  Expr *value = stmt->value;
  if (!value) {
    emitReturnOf(nullptr);
    return;
  }
  if (value->type->isPromise()) {
    // `return p`: the function's promise follows p, a turn of the queue
    // later, as in JavaScript. Through a `finally`, which runs first, it
    // waits for p here instead.
    if (!finallyBetween(~size_t(0))) {
      llvm::Value *other = emitExpr(value);
      emitCleanups(0);
      followPromise(async_->object, other);
      builder_.CreateBr(coro_->finalBlock);
      startDeadBlock();
      return;
    }
    llvm::Value *result = awaitPromise(emitExpr(value), value->type, stmt->loc());
    emitReturnOf(promiseValueSize(async_->value) ? result : nullptr);
    return;
  }
  llvm::Value *result = emitExpr(value);
  if (!promiseValueSize(async_->value) && !value->type->isVoid()) {
    releaseValue(result, value->type); // a callback whose promise is dropped
    result = nullptr;
  }
  emitReturnOf(value->type->isVoid() ? nullptr : result);
}

// The places of an error that comes out of an `await`: "async f", as V8
// names them.
llvm::Constant *CodeGen::asyncFrameDescriptor(llvm::SMLoc loc) {
  std::string name = frameFunctionName();
  name = name.empty() ? "async" : "async " + name;
  return frameDescriptor(loc, &name);
}

// Waits for the coroutine's turn: the runtime queues its reactor once
// `promise` (null: none) settles.
void CodeGen::suspendAsync(llvm::Value *promise, llvm::SMLoc loc) {
  llvm::Type *ptr = builder_.getPtrTy();
  // Where top-level code waits, for the warning about an await that never
  // ends.
  if (!currentFn_) {
    auto *where = cast<llvm::GlobalVariable>(module_->getOrInsertGlobal("tz_main_await", ptr));
    builder_.CreateStore(frameDescriptor(loc), where);
  }
  builder_.CreateCall(asyncFunction("tz_async_await", builder_.getVoidTy(), {ptr, ptr}),
                      {async_->reactor, promise ? promise : llvm::ConstantPointerNull::get(ptr)});
  emitSuspend();
}

llvm::Value *CodeGen::awaitPromise(llvm::Value *promise, const Type *type, llvm::SMLoc loc) {
  const Type *value = type->promised();
  suspendAsync(promise, loc);
  llvm::Value *state =
      builder_.CreateLoad(builder_.getInt32Ty(), promiseField(promise, kState), "state");
  llvm::BasicBlock *fulfilledBlock = newBlock("await.fulfilled");
  llvm::BasicBlock *rejectedBlock = newBlock("await.rejected");
  builder_.CreateCondBr(builder_.CreateICmpEQ(state, builder_.getInt32(kFulfilled)), fulfilledBlock,
                        rejectedBlock, llvm::MDBuilder(ctx_).createLikelyBranchWeights());
  // Rejected: its error goes up from here, with this place.
  switchTo(rejectedBlock);
  llvm::Value *error =
      builder_.CreateLoad(builder_.getPtrTy(), promiseField(promise, kError), "error");
  retainObject(error);
  releaseValue(promise, type);
  builder_.CreateStore(error, thrownGlobal());
  builder_.CreateCall(
      processFunction("tz_error_frame", builder_.getVoidTy(), {builder_.getPtrTy()}),
      {asyncFrameDescriptor(loc)});
  builder_.CreateBr(unwindTarget());
  switchTo(fulfilledBlock);
  llvm::Value *result = nullptr;
  if (promiseValueSize(value)) {
    result = builder_.CreateLoad(lower(value), promiseValueAddress(promise), "awaited");
    retainValue(result, value);
  }
  releaseValue(promise, type);
  return result;
}

llvm::Value *CodeGen::emitAwait(AwaitExpr *await) {
  Expr *operand = await->value;
  // `await Promise.resolve(x)` waits as `await x` does, one turn of the
  // queue, without the promise.
  if (const Expr *inner = resolvedValue(operand))
    operand = const_cast<Expr *>(inner);
  const Type *type = operand->type;
  llvm::Value *value = emitExpr(operand);
  if (type->isPromise())
    return awaitPromise(value, type, await->loc());
  if (holdsPromise(type))
    return awaitUnion(value, type, await->type, await->loc());
  suspendAsync(nullptr, await->loc());
  return type->isVoid() ? nullptr : value;
}

bool CodeGen::holdsPromise(const Type *type) {
  type = type->nonOptional();
  if (type->isPromise())
    return true;
  return type->isUnion() && llvm::any_of(type->members(), [](const Type *m) { return m->isPromise(); });
}

// `await` of a union (or `T | undefined`) that may hold a Promise, as in
// JavaScript: it waits for the Promise, or a turn of the queue for any
// other value.
llvm::Value *CodeGen::awaitUnion(llvm::Value *value, const Type *type, const Type *result,
                                 llvm::SMLoc loc) {
  llvm::Type *resultType = result->isVoid() ? nullptr : lower(result);
  auto plain = [&](llvm::Value *v, const Type *t) -> llvm::Value * {
    suspendAsync(nullptr, loc);
    if (!resultType) {
      if (v)
        releaseValue(v, t);
      return nullptr;
    }
    return v ? emitConversion(v, t, result) : undefinedValue(result);
  };
  auto promise = [&](llvm::Value *v, const Type *t) -> llvm::Value * {
    llvm::Value *got = awaitPromise(v, t, loc);
    if (!resultType) {
      if (got)
        releaseValue(got, t->promised());
      return nullptr;
    }
    return emitConversion(got, t->promised(), result);
  };
  auto part = [&](llvm::Value *v, const Type *t) -> llvm::Value * {
    if (t->isPromise())
      return promise(v, t);
    if (holdsPromise(t))
      return awaitUnion(v, t, result, loc);
    return plain(v, t);
  };
  if (type->isTagged())
    return forEachSlot(
        value, type->slots(),
        [&](UnionSlot slot) { return part(taggedPart(value, type, slot), type->slotPart(slot)); },
        resultType, resultType ? llvm::Constant::getNullValue(resultType) : nullptr);
  llvm::BasicBlock *endBlock = newBlock("await.end");
  std::vector<std::pair<llvm::Value *, llvm::BasicBlock *>> incoming;
  auto finish = [&](llvm::Value *v) {
    incoming.push_back({v, builder_.GetInsertBlock()});
    builder_.CreateBr(endBlock);
  };
  llvm::BasicBlock *promiseBlock = newBlock("await.promise");
  llvm::BasicBlock *otherBlock = newBlock("await.other");
  if (type->isOptional()) {
    builder_.CreateCondBr(optionalHas(value, type), promiseBlock, otherBlock);
    switchTo(otherBlock);
    finish(plain(nullptr, type));
    switchTo(promiseBlock);
    finish(part(optionalValue(value, type), type->inner()));
  } else {
    // Objects, one of them a Promise: told apart by the number of its layout.
    const Type *promiseType = *llvm::find_if(type->members(), [](const Type *m) { return m->isPromise(); });
    builder_.CreateCondBr(emitIsOneOf(objectClassId(value), promiseType), promiseBlock, otherBlock);
    switchTo(promiseBlock);
    finish(promise(value, promiseType));
    switchTo(otherBlock);
    finish(plain(value, unionWithout(type, promiseType)));
  }
  switchTo(endBlock);
  if (!resultType)
    return nullptr;
  llvm::PHINode *phi = builder_.CreatePHI(resultType, unsigned(incoming.size()));
  for (auto [v, block] : incoming)
    phi->addIncoming(v, block);
  return phi;
}

//===----------------------------------------------------------------------===//
// new Promise, the functions that settle it, and the functions of Promise
//===----------------------------------------------------------------------===//

// void (ptr promise, T value): `resolve` of `new Promise`, whose environment
// is the promise. Only the first of resolve and reject counts.
llvm::Constant *CodeGen::resolverInfo(const Type *value) {
  llvm::Constant *&info = resolverInfos_[value];
  if (info)
    return info;
  llvm::Type *ptr = builder_.getPtrTy();
  bool holds = promiseValueSize(value) != 0;
  std::vector<llvm::Type *> params{ptr};
  if (!value->isVoid())
    params.push_back(lower(value));
  auto *code = llvm::Function::Create(llvm::FunctionType::get(builder_.getVoidTy(), params, false),
                                      llvm::GlobalValue::InternalLinkage, "tz.promise.resolve",
                                      module_.get());
  setFunctionAttributes(code);
  llvm::IRBuilderBase::InsertPoint point = builder_.saveIP();
  llvm::Function *outer = std::exchange(function_, code);
  builder_.SetInsertPoint(llvm::BasicBlock::Create(ctx_, "entry", code));
  llvm::Value *promise = code->getArg(0);
  llvm::Value *locked = builder_.CreateCall(
      asyncFunction("tz_promise_lock", builder_.getInt32Ty(), {ptr}), {promise}, "locked");
  llvm::BasicBlock *settleBlock = newBlock("settle");
  llvm::BasicBlock *dropBlock = newBlock("drop");
  builder_.CreateCondBr(builder_.CreateIsNotNull(locked), settleBlock, dropBlock);
  switchTo(settleBlock);
  fulfillPromise(promise, holds ? code->getArg(1) : nullptr, value);
  builder_.CreateRetVoid();
  switchTo(dropBlock);
  if (!value->isVoid())
    releaseValue(code->getArg(1), value);
  builder_.CreateRetVoid();
  function_ = outer;
  builder_.restoreIP(point);
  info = new llvm::GlobalVariable(
      *module_, fnInfoTy_, /*isConstant=*/true, llvm::GlobalValue::PrivateLinkage,
      llvm::ConstantStruct::get(fnInfoTy_, {code, stringLiteral(u"")}), "tz.fninfo.resolve");
  return info;
}

// void (ptr promise, ptr error): `reject`.
llvm::Constant *CodeGen::rejecterInfo() {
  if (rejecterInfo_)
    return rejecterInfo_;
  llvm::Type *ptr = builder_.getPtrTy();
  auto *code = llvm::Function::Create(
      llvm::FunctionType::get(builder_.getVoidTy(), {ptr, ptr}, false),
      llvm::GlobalValue::InternalLinkage, "tz.promise.reject", module_.get());
  setFunctionAttributes(code);
  llvm::IRBuilderBase::InsertPoint point = builder_.saveIP();
  llvm::Function *outer = std::exchange(function_, code);
  builder_.SetInsertPoint(llvm::BasicBlock::Create(ctx_, "entry", code));
  llvm::Value *promise = code->getArg(0), *error = code->getArg(1);
  llvm::Value *locked = builder_.CreateCall(
      asyncFunction("tz_promise_lock", builder_.getInt32Ty(), {ptr}), {promise}, "locked");
  llvm::BasicBlock *settleBlock = newBlock("settle");
  llvm::BasicBlock *dropBlock = newBlock("drop");
  builder_.CreateCondBr(builder_.CreateIsNotNull(locked), settleBlock, dropBlock);
  switchTo(settleBlock);
  rejectPromise(promise, error);
  builder_.CreateRetVoid();
  switchTo(dropBlock);
  builder_.CreateCall(instanceReleaseFn_, {error});
  builder_.CreateRetVoid();
  function_ = outer;
  builder_.restoreIP(point);
  rejecterInfo_ = new llvm::GlobalVariable(
      *module_, fnInfoTy_, /*isConstant=*/true, llvm::GlobalValue::PrivateLinkage,
      llvm::ConstantStruct::get(fnInfoTy_, {code, stringLiteral(u"")}), "tz.fninfo.reject");
  return rejecterInfo_;
}

// The executor runs now with the two functions; an error it throws rejects
// the promise, unless it is settled already.
llvm::Value *CodeGen::emitPromiseNew(CallExpr *call) {
  const Type *value = call->type->promised();
  Expr *executorExpr = call->args[0];
  const Type *fnType = executorExpr->type;
  llvm::Value *executor = emitExpr(executorExpr);
  llvm::Value *promise = newPromise(value);
  std::vector<llvm::Value *> args;
  // The promise may be a member of a group (promiseIsMember): counted as
  // one.
  for (int i = 0; i < 2; ++i) {
    builder_.CreateCall(objRetainFn_, {promise});
    args.push_back(makeFunctionValue(i == 0 ? resolverInfo(value) : rejecterInfo(), promise));
  }
  // An executor that names fewer parameters gets fewer.
  size_t takes = std::min<size_t>(fnType->params().size(), 2);
  for (size_t i = takes; i < args.size(); ++i)
    builder_.CreateCall(instanceReleaseFn_, {promise});
  args.resize(takes);
  callFunctionValue(executor, fnType, args);
  bool throws = typeThrows(fnType);
  if (auto *fe = dyn_cast<FunctionExpr>(executorExpr))
    throws = mayThrow(fe->fn);
  if (throws) {
    llvm::Value *error = builder_.CreateLoad(builder_.getPtrTy(), thrownGlobal(), "thrown");
    llvm::BasicBlock *rejectBlock = newBlock("executor.threw");
    llvm::BasicBlock *doneBlock = newBlock("executor.done");
    builder_.CreateCondBr(builder_.CreateIsNotNull(error), rejectBlock, doneBlock,
                          llvm::MDBuilder(ctx_).createUnlikelyBranchWeights());
    switchTo(rejectBlock);
    builder_.CreateStore(llvm::ConstantPointerNull::get(builder_.getPtrTy()), thrownGlobal());
    // As reject does: nothing once the promise is settled.
    llvm::Value *locked = builder_.CreateCall(
        asyncFunction("tz_promise_lock", builder_.getInt32Ty(), {builder_.getPtrTy()}), {promise},
        "locked");
    llvm::BasicBlock *settleBlock = newBlock("executor.reject");
    llvm::BasicBlock *dropBlock = newBlock("executor.drop");
    builder_.CreateCondBr(builder_.CreateIsNotNull(locked), settleBlock, dropBlock);
    switchTo(settleBlock);
    rejectPromise(promise, error);
    builder_.CreateBr(doneBlock);
    switchTo(dropBlock);
    builder_.CreateCall(instanceReleaseFn_, {error});
    builder_.CreateBr(doneBlock);
    switchTo(doneBlock);
  }
  releaseValue(executor, fnType);
  return promise;
}

llvm::Value *CodeGen::emitPromiseStatic(CallExpr *call) {
  const Type *type = call->type;
  llvm::Type *ptr = builder_.getPtrTy();
  switch (call->builtin) {
  case Builtin::PromiseResolve: {
    if (!call->args.empty() && call->args[0]->type->isPromise())
      return emitExpr(call->args[0]); // that very promise
    llvm::Value *value = nullptr;
    if (!call->args.empty())
      value = emitExpr(call->args[0]);
    auto *fn = cast<llvm::Function>(asyncFunction("tz_promise_resolved", ptr, {ptr}).getCallee());
    llvm::Value *promise = builder_.CreateCall(fn, {promiseLayout(type->promised())}, "promise");
    if (value && promiseValueSize(type->promised()))
      builder_.CreateStore(value, promiseValueAddress(promise));
    return promise;
  }
  case Builtin::PromiseReject: {
    llvm::Value *error = emitExpr(call->args[0]);
    builder_.CreateCall(processFunction("tz_error_rejected", builder_.getVoidTy(), {ptr, ptr}),
                        {error, frameDescriptor(loc_)});
    return builder_.CreateCall(asyncFunction("tz_promise_rejected", ptr, {ptr, ptr}),
                               {promiseLayout(type->promised()), error}, "promise");
  }
  case Builtin::PromiseWithResolvers: {
    const Type *promiseType = type->field("promise")->type;
    const Type *value = promiseType->promised();
    llvm::Value *promise = newPromise(value);
    llvm::Value *object = allocObject(type, {0, 1, 2});
    builder_.CreateStore(promise, fieldAddress(object, type, "promise"));
    for (int i = 0; i < 2; ++i) {
      builder_.CreateCall(objRetainFn_, {promise});
      builder_.CreateStore(
          makeFunctionValue(i == 0 ? resolverInfo(value) : rejecterInfo(), promise),
          fieldAddress(object, type, i == 0 ? "resolve" : "reject"));
    }
    adoptFields(object, type);
    return object;
  }
  default:
    return emitPromiseCombine(call);
  }
}

// Promise.all, allSettled, race and any: the runtime gets an array of the
// promises, a value that is none becoming a resolved one, as in JavaScript.
llvm::Value *CodeGen::emitPromiseCombine(CallExpr *call) {
  llvm::Type *ptr = builder_.getPtrTy(), *i32 = builder_.getInt32Ty(), *i64 = builder_.getInt64Ty();
  const Type *anyPromise = Type::promise(Type::voidTy());
  Expr *arg = call->args[0];
  const Type *argType = arg->type;
  auto toPromise = [&](llvm::Value *value, const Type *type) -> llvm::Value * {
    if (type->isPromise())
      return value;
    auto *fn = cast<llvm::Function>(asyncFunction("tz_promise_resolved", ptr, {ptr}).getCallee());
    llvm::Value *promise = builder_.CreateCall(fn, {promiseLayout(type)}, "promise");
    if (promiseValueSize(type))
      builder_.CreateStore(value, promiseValueAddress(promise));
    return promise;
  };
  // The values, each made a promise, in a new array.
  auto arrayOf = [&](std::vector<llvm::Value *> values, llvm::ArrayRef<const Type *> types) {
    llvm::Value *array =
        builder_.CreateCall(rt_.arrNew,
                            {typeDescriptor(anyPromise), builder_.getInt64(values.size()),
                             builder_.getInt64(values.size())},
                            "promises");
    llvm::Value *data = arrayData(array);
    for (size_t i = 0; i < values.size(); ++i)
      builder_.CreateStore(toPromise(values[i], types[i]),
                           builder_.CreateConstInBoundsGEP1_64(ptr, data, i));
    return array;
  };
  llvm::Value *promises;
  // What each promise of a tuple gives, which may differ.
  std::vector<const Type *> given;
  // A literal of the values themselves; `[...xs]` makes an array first.
  auto *lit = dyn_cast<ArrayLitExpr>(arg);
  if (lit && llvm::any_of(lit->elements,
                          [](const ArrayLitExpr::Element &e) { return e.spread || e.hole; }))
    lit = nullptr;
  if (lit) {
    std::vector<Expr *> exprs;
    std::vector<const Type *> types;
    for (ArrayLitExpr::Element &e : lit->elements) {
      exprs.push_back(e.expr);
      types.push_back(e.expr->type);
    }
    if (argType->isTuple())
      given = types;
    promises = arrayOf(emitHeldValues(exprs), types);
  } else if (argType->isTuple()) {
    llvm::Value *tuple = emitExpr(arg);
    std::vector<llvm::Value *> values;
    std::vector<const Type *> types;
    for (const Field &f : argType->fields())
      given.push_back(f.type);
    for (const Field &f : argType->fields()) {
      llvm::Value *v = builder_.CreateLoad(lower(f.type), fieldAddress(tuple, argType, f.name));
      retainValue(v, f.type);
      values.push_back(v);
      types.push_back(f.type);
    }
    releaseValue(tuple, argType);
    promises = arrayOf(values, types);
  } else if (argType->inner()->isPromise()) {
    promises = emitExpr(arg);
  } else {
    const Type *element = argType->inner();
    llvm::Value *values = emitExpr(arg);
    promises = builder_.CreateCall(asyncFunction("tz_promise_resolve_all", ptr, {ptr, ptr, ptr}),
                                   {values, promiseLayout(element), typeDescriptor(anyPromise)},
                                   "promises");
    releaseValue(values, argType);
  }

  // How the runtime collects what it gives (TzCombine in runtime/tzrt.h).
  const Type *result = call->type->promised();
  int32_t kind = call->builtin == Builtin::PromiseAll          ? kAll
                 : call->builtin == Builtin::PromiseAllSettled ? kAllSettled
                 : call->builtin == Builtin::PromiseRace       ? kRace
                                                               : kAny;
  llvm::Constant *null = llvm::ConstantPointerNull::get(builder_.getPtrTy());
  llvm::Constant *items = null, *tuple = null, *convert = null, *each = null;
  uint32_t tupleSize = 0;
  const llvm::DataLayout &dl = module_->getDataLayout();
  auto *shapeType =
      llvm::StructType::get(ctx_, {ptr, ptr, ptr, i32, i32, ptr, ptr, i32, i32, i32, ptr, ptr});
  // The layout, size, value type and field offsets of allSettled's result
  // object.
  struct Settled {
    llvm::Constant *layout, *valueType;
    uint32_t size = 0, status = 0, value = 0, reason = 0;
  };
  auto settledOf = [&](const Type *object) {
    const ObjectLayout &layout = objectLayout(object);
    const llvm::StructLayout *offsets = dl.getStructLayout(layout.type);
    auto offset = [&](llvm::StringRef name) {
      return uint32_t(offsets->getElementOffset(layout.elements[fieldIndex(object, name)]));
    };
    std::vector<unsigned> keys{fieldIndex(object, "status")};
    if (object->field("value"))
      keys.push_back(fieldIndex(object, "value"));
    keys.push_back(fieldIndex(object, "reason"));
    Settled s{layoutDescriptor(object, keys), null};
    s.size = uint32_t(dl.getTypeAllocSize(layout.type));
    s.status = offset("status");
    s.reason = offset("reason");
    if (const Field *value = object->field("value")) {
      s.value = offset("value");
      s.valueType = typeDescriptor(value->type);
    }
    return s;
  };
  auto shapeOf = [&](llvm::Constant *resultLayout, const Settled &s) {
    return llvm::ConstantStruct::get(
        shapeType, {resultLayout, items, tuple, builder_.getInt32(tupleSize),
                    builder_.getInt32(s.size), s.layout, s.valueType, builder_.getInt32(s.status),
                    builder_.getInt32(s.value), builder_.getInt32(s.reason), convert, each});
  };
  Settled settled{null, null};
  if (result->isTuple()) {
    std::vector<unsigned> keys(result->fields().size());
    for (unsigned i = 0; i < keys.size(); ++i)
      keys[i] = i;
    tuple = layoutDescriptor(result, keys);
    tupleSize = uint32_t(dl.getTypeAllocSize(objectLayout(result).type));
  } else if (result->isArray()) {
    items = typeDescriptor(result->inner());
  }
  if (kind == kAllSettled && result->isTuple()) {
    // A tuple of results, each of its own type.
    std::vector<llvm::Constant *> shapes;
    for (const Field &f : result->fields())
      shapes.push_back(shapeOf(null, settledOf(f.type)));
    auto *eachType = llvm::ArrayType::get(shapeType, shapes.size());
    auto *global = new llvm::GlobalVariable(
        *module_, eachType, /*isConstant=*/true, llvm::GlobalValue::PrivateLinkage,
        llvm::ConstantArray::get(eachType, shapes), "tz.promise.settled");
    global->setUnnamedAddr(llvm::GlobalValue::UnnamedAddr::Global);
    each = global;
  } else if (kind == kAllSettled) {
    settled = settledOf(result->inner());
  }
  if ((kind == kRace || kind == kAny) && promiseValueSize(result)) {
    // Promises of different values give one of a union.
    std::vector<llvm::Constant *> fns;
    bool converts = false;
    for (const Type *t : given) {
      const Type *value = t->isPromise() ? t->promised() : t;
      if (value == result || value->isNever() || !promiseValueSize(value)) {
        fns.push_back(null);
        continue;
      }
      fns.push_back(combineConverter(value, result));
      converts = true;
    }
    if (converts) {
      auto *fnsType = llvm::ArrayType::get(ptr, fns.size());
      auto *global = new llvm::GlobalVariable(
          *module_, fnsType, /*isConstant=*/true, llvm::GlobalValue::PrivateLinkage,
          llvm::ConstantArray::get(fnsType, fns), "tz.promise.convert");
      global->setUnnamedAddr(llvm::GlobalValue::UnnamedAddr::Global);
      convert = global;
    }
  }
  auto *shape = new llvm::GlobalVariable(*module_, shapeType, /*isConstant=*/true,
                                         llvm::GlobalValue::PrivateLinkage,
                                         shapeOf(promiseLayout(result), settled),
                                         "tz.promise.combine");
  shape->setUnnamedAddr(llvm::GlobalValue::UnnamedAddr::Global);
  (void)i64;
  llvm::Value *promise =
      builder_.CreateCall(asyncFunction("tz_promise_combine", ptr, {i32, ptr, ptr}),
                          {builder_.getInt32(uint32_t(kind)), promises, shape}, "promise");
  // The array may be a member of a group, as its promises may be.
  builder_.CreateCall(hasCycles_ ? groupAwareRelease(arrReleaseFn_) : arrReleaseFn_, {promises});
  return promise;
}

llvm::Function *CodeGen::combineConverter(const Type *from, const Type *to) {
  llvm::Function *&fn = combineConverters_[{from, to}];
  if (fn)
    return fn;
  llvm::Type *ptr = builder_.getPtrTy();
  fn = llvm::Function::Create(llvm::FunctionType::get(builder_.getVoidTy(), {ptr, ptr}, false),
                              llvm::GlobalValue::InternalLinkage, "tz.promise.convert",
                              module_.get());
  setFunctionAttributes(fn);
  llvm::IRBuilderBase::InsertPoint point = builder_.saveIP();
  llvm::Function *outer = std::exchange(function_, fn);
  builder_.SetInsertPoint(llvm::BasicBlock::Create(ctx_, "entry", fn));
  llvm::Value *value = builder_.CreateLoad(lower(from), fn->getArg(1), "value");
  retainValue(value, from);
  builder_.CreateStore(emitConversion(value, from, to), fn->getArg(0));
  builder_.CreateRetVoid();
  function_ = outer;
  builder_.restoreIP(point);
  return fn;
}

//===----------------------------------------------------------------------===//
// then, catch and finally
//===----------------------------------------------------------------------===//

llvm::StructType *CodeGen::reactionType() {
  if (!reactionTy_) {
    llvm::Type *ptr = builder_.getPtrTy();
    reactionTy_ = llvm::StructType::create(
        ctx_, {builder_.getInt64Ty(), ptr, ptr, ptr, ptr, ptr, ptr, fnTy_, fnTy_}, "tz.reaction");
  }
  return reactionTy_;
}

// Releases the promise a reaction gives and its callbacks, then frees it.
llvm::Function *CodeGen::reactionDrop() {
  if (reactionDropFn_)
    return reactionDropFn_;
  llvm::Type *ptr = builder_.getPtrTy();
  reactionDropFn_ =
      llvm::Function::Create(llvm::FunctionType::get(builder_.getVoidTy(), {ptr}, false),
                             llvm::GlobalValue::InternalLinkage, "tz.reaction.drop", module_.get());
  reactionDropFn_->addFnAttr(llvm::Attribute::NoUnwind);
  llvm::IRBuilderBase::InsertPoint point = builder_.saveIP();
  builder_.SetInsertPoint(llvm::BasicBlock::Create(ctx_, "entry", reactionDropFn_));
  llvm::Value *object = reactionDropFn_->getArg(0);
  llvm::StructType *type = reactionType();
  llvm::Value *derived = builder_.CreateLoad(ptr, builder_.CreateStructGEP(type, object, kReactionDerived));
  if (hasCycles_) {
    // A member of a group (attachReaction): what it holds goes as from one.
    const Type *anyFunction = Type::function({}, 0, false, Type::voidTy());
    releaseIn(object, derived, Type::promise(Type::voidTy()));
    for (unsigned field : {kReactionFulfilled, kReactionRejected})
      releaseIn(object, builder_.CreateLoad(fnTy_, builder_.CreateStructGEP(type, object, field)),
                anyFunction);
    builder_.CreateCall(
        groupFunction("tz_group_obj_free", builder_.getVoidTy(), {builder_.getPtrTy()}), {object});
  } else {
    builder_.CreateCall(instanceReleaseFn_, {derived});
    for (unsigned field : {kReactionFulfilled, kReactionRejected}) {
      llvm::Value *fn = builder_.CreateLoad(fnTy_, builder_.CreateStructGEP(type, object, field));
      builder_.CreateCall(objReleaseFn_, {builder_.CreateExtractValue(fn, 1)});
    }
    builder_.CreateCall(rt_.objFree, {object});
  }
  builder_.CreateRetVoid();
  builder_.restoreIP(point);
  return reactionDropFn_;
}

// void (ptr reactor, ptr source): the code of a reaction, once the promise
// it waits for (`source`, of a value of `value`) settled. It calls the
// callback for how it settled, if there is one, and settles its own promise,
// of a value of `result`, with what that gives, or as `source` did.
llvm::Function *CodeGen::reactionHandler(Builtin kind, const Type *value, const Type *result,
                                         const Type *onFulfilled, const Type *onRejected) {
  llvm::Function *&handler = reactionHandlers_[{kind, value, result, onFulfilled, onRejected}];
  if (handler)
    return handler;
  llvm::Type *ptr = builder_.getPtrTy();
  handler = llvm::Function::Create(
      llvm::FunctionType::get(builder_.getVoidTy(), {ptr, ptr}, false),
      llvm::GlobalValue::InternalLinkage,
      kind == Builtin::PromiseFinally ? "tz.promise.finally" : "tz.promise.then", module_.get());
  setFunctionAttributes(handler);
  llvm::IRBuilderBase::InsertPoint point = builder_.saveIP();
  llvm::Function *outer = std::exchange(function_, handler);
  builder_.SetInsertPoint(llvm::BasicBlock::Create(ctx_, "entry", handler));
  llvm::StructType *type = reactionType();
  llvm::Value *object = builder_.CreateInBoundsGEP(
      builder_.getInt8Ty(), handler->getArg(0),
      llvm::ConstantInt::getSigned(builder_.getInt64Ty(), -int64_t(kReactorOffset)), "reaction");
  llvm::Value *source = handler->getArg(1);
  llvm::Value *derived =
      builder_.CreateLoad(ptr, builder_.CreateStructGEP(type, object, kReactionDerived), "derived");
  bool holds = promiseValueSize(result) != 0;

  // What a callback of type `fnType` gave: its error, a promise to follow, or
  // the value.
  auto settleWith = [&](llvm::Value *given, const Type *fnType) {
    if (typeThrows(fnType)) {
      llvm::Value *error = builder_.CreateLoad(ptr, thrownGlobal(), "thrown");
      llvm::BasicBlock *threwBlock = newBlock("callback.threw");
      llvm::BasicBlock *gaveBlock = newBlock("callback.gave");
      builder_.CreateCondBr(builder_.CreateIsNotNull(error), threwBlock, gaveBlock,
                            llvm::MDBuilder(ctx_).createUnlikelyBranchWeights());
      switchTo(threwBlock);
      builder_.CreateStore(llvm::ConstantPointerNull::get(ptr), thrownGlobal());
      rejectPromise(derived, error);
      builder_.CreateRetVoid();
      switchTo(gaveBlock);
    }
    const Type *ret = fnType->ret();
    if (ret->isPromise()) {
      followPromise(derived, given);
    } else {
      llvm::Value *converted = nullptr;
      if (holds && !ret->isVoid())
        converted = emitConversion(given, ret, result);
      fulfillPromise(derived, converted, result);
    }
    builder_.CreateRetVoid();
  };
  // Calls the callback in `field` of type `fnType` with `arg` (owned; null
  // for none), as many as it takes.
  auto callWith = [&](unsigned field, const Type *fnType, llvm::Value *arg, const Type *argType) {
    llvm::Value *fn =
        builder_.CreateLoad(fnTy_, builder_.CreateStructGEP(type, object, field), "callback");
    std::vector<llvm::Value *> args;
    if (arg && !fnType->params().empty())
      args.push_back(arg);
    else if (arg)
      releaseValue(arg, argType);
    return callFunctionValue(fn, fnType, args);
  };

  if (kind == Builtin::PromiseFinally) {
    llvm::Value *given = callWith(kReactionFulfilled, onFulfilled, nullptr, nullptr);
    if (typeThrows(onFulfilled)) {
      llvm::Value *error = builder_.CreateLoad(ptr, thrownGlobal(), "thrown");
      llvm::BasicBlock *threwBlock = newBlock("callback.threw");
      llvm::BasicBlock *gaveBlock = newBlock("callback.gave");
      builder_.CreateCondBr(builder_.CreateIsNotNull(error), threwBlock, gaveBlock,
                            llvm::MDBuilder(ctx_).createUnlikelyBranchWeights());
      switchTo(threwBlock);
      builder_.CreateStore(llvm::ConstantPointerNull::get(ptr), thrownGlobal());
      rejectPromise(derived, error);
      builder_.CreateRetVoid();
      switchTo(gaveBlock);
    }
    llvm::Value *awaited =
        onFulfilled->ret()->isPromise() ? given : llvm::ConstantPointerNull::get(ptr);
    builder_.CreateCall(asyncFunction("tz_promise_finally", builder_.getVoidTy(), {ptr, ptr, ptr}),
                        {derived, source, awaited});
    builder_.CreateRetVoid();
  } else {
    llvm::Value *state =
        builder_.CreateLoad(builder_.getInt32Ty(), promiseField(source, kState), "state");
    llvm::BasicBlock *fulfilledBlock = newBlock("fulfilled");
    llvm::BasicBlock *rejectedBlock = newBlock("rejected");
    builder_.CreateCondBr(builder_.CreateICmpEQ(state, builder_.getInt32(kFulfilled)),
                          fulfilledBlock, rejectedBlock);
    switchTo(fulfilledBlock);
    llvm::Value *given = nullptr;
    if (promiseValueSize(value)) {
      given = builder_.CreateLoad(lower(value), promiseValueAddress(source), "value");
      retainValue(given, value);
    }
    if (onFulfilled) {
      settleWith(callWith(kReactionFulfilled, onFulfilled, given, value), onFulfilled);
    } else {
      fulfillPromise(derived, holds && given ? emitConversion(given, value, result) : nullptr,
                     result);
      builder_.CreateRetVoid();
    }
    switchTo(rejectedBlock);
    llvm::Value *error = builder_.CreateLoad(ptr, promiseField(source, kError), "error");
    retainObject(error);
    if (onRejected) {
      settleWith(callWith(kReactionRejected, onRejected, error, errorClass_->type), onRejected);
    } else {
      rejectPromise(derived, error);
      builder_.CreateRetVoid();
    }
  }
  function_ = outer;
  builder_.restoreIP(point);
  return handler;
}

llvm::Value *CodeGen::emitPromiseReaction(CallExpr *call) {
  auto *member = cast<MemberExpr>(call->callee);
  const Type *promiseType = member->object->type;
  const Type *value = promiseType->promised(), *result = call->type->promised();
  std::vector<Expr *> exprs{member->object};
  exprs.insert(exprs.end(), call->args.begin(), call->args.end());
  std::vector<llvm::Value *> values = emitHeldValues(exprs);
  llvm::Value *source = values[0];
  // The callbacks, by what they are for; `undefined` is none.
  const Type *types[2] = {nullptr, nullptr};
  llvm::Value *callbacks[2] = {nullptr, nullptr};
  size_t first = call->builtin == Builtin::PromiseCatch ? 1 : 0;
  for (size_t i = 0; i < call->args.size(); ++i) {
    if (!call->args[i]->type->isFunction())
      continue;
    types[first + i] = call->args[i]->type;
    callbacks[first + i] = values[i + 1];
  }
  llvm::Value *derived = newPromise(result);
  attachReaction(source, derived, reactionHandler(call->builtin, value, result, types[0], types[1]),
                 callbacks[0], callbacks[1]);
  releaseValue(source, promiseType);
  return derived;
}

void CodeGen::attachReaction(llvm::Value *source, llvm::Value *derived, llvm::Function *handler,
                             llvm::Value *onFulfilled, llvm::Value *onRejected, bool later) {
  llvm::StructType *type = reactionType();
  uint64_t size = module_->getDataLayout().getTypeAllocSize(type);
  // In a program with cycles, a member of a group, as what holds it (its
  // promise, see tz_promise_react) and what it holds may be: else a cycle
  // through it could never be freed.
  llvm::Value *object = hasCycles_
                            ? allocMember(size, "reaction")
                            : builder_.CreateCall(rt_.objAlloc, {builder_.getInt64(size)}, "reaction");
  llvm::Value *null = llvm::ConstantPointerNull::get(builder_.getPtrTy());
  if (!hasCycles_)
    builder_.CreateStore(builder_.getInt64(1), builder_.CreateStructGEP(type, object, kReactionRc));
  // A member's descriptor is a layout, which the report of debug mode reads.
  builder_.CreateStore(hasCycles_ ? memberDescriptor(reactionDrop(), "reaction")
                                  : dropDescriptor(reactionDrop()),
                       builder_.CreateStructGEP(type, object, kReactionDesc));
  builder_.CreateStore(null, builder_.CreateStructGEP(type, object, kReactorNext));
  builder_.CreateStore(handler, builder_.CreateStructGEP(type, object, kReactorRun));
  builder_.CreateStore(object, builder_.CreateStructGEP(type, object, kReactorOwner));
  builder_.CreateStore(null, builder_.CreateStructGEP(type, object, kReactorSource));
  builder_.CreateCall(objRetainFn_, {derived});
  builder_.CreateStore(derived, builder_.CreateStructGEP(type, object, kReactionDerived));
  llvm::Value *callbacks[2] = {onFulfilled, onRejected};
  for (unsigned i = 0; i < 2; ++i)
    builder_.CreateStore(
        callbacks[i] ? callbacks[i] : llvm::Constant::getNullValue(fnTy_),
        builder_.CreateStructGEP(type, object, i == 0 ? kReactionFulfilled : kReactionRejected));
  if (hasCycles_) {
    groupStore(object, derived, Type::promise(Type::voidTy()));
    for (llvm::Value *callback : callbacks)
      if (callback)
        groupStore(object, callback, Type::function({}, 0, false, Type::voidTy()));
  }
  llvm::Type *ptr = builder_.getPtrTy();
  builder_.CreateCall(asyncFunction(later ? "tz_promise_react_later" : "tz_promise_react",
                                    builder_.getVoidTy(), {ptr, ptr}),
                      {source, builder_.CreateStructGEP(type, object, kReactorNext)});
}

//===----------------------------------------------------------------------===//
// ResultAsync of neverthrow
//===----------------------------------------------------------------------===//

// The code of a reaction of a ResultAsync, once the promise it waits for
// (`source`, of a value of `value`) settled: it settles its own promise, of
// a value of `result`, as the method `kind` does with the callbacks of types
// `first` and `second`. It takes the turns of the queue that neverthrow's
// code takes, so that what else runs meanwhile runs in the same order: map,
// mapErr, orElse and the tees run an async function, whose promise the
// ResultAsync then follows; andThen, match and unwrapOr are a callback of
// then(); fromPromise is a then() and a catch().
llvm::Function *CodeGen::resultAsyncHandler(Builtin kind, const Type *value, const Type *result,
                                            const Type *first, const Type *second) {
  llvm::Function *&handler = reactionHandlers_[{kind, value, result, first, second}];
  if (handler)
    return handler;
  llvm::Type *ptr = builder_.getPtrTy();
  handler =
      llvm::Function::Create(llvm::FunctionType::get(builder_.getVoidTy(), {ptr, ptr}, false),
                             llvm::GlobalValue::InternalLinkage, "tz.result.async", module_.get());
  setFunctionAttributes(handler);
  llvm::IRBuilderBase::InsertPoint point = builder_.saveIP();
  llvm::Function *outer = std::exchange(function_, handler);
  builder_.SetInsertPoint(llvm::BasicBlock::Create(ctx_, "entry", handler));
  llvm::StructType *type = reactionType();
  llvm::Value *object = builder_.CreateInBoundsGEP(
      builder_.getInt8Ty(), handler->getArg(0),
      llvm::ConstantInt::getSigned(builder_.getInt64Ty(), -int64_t(kReactorOffset)), "reaction");
  llvm::Value *source = handler->getArg(1);
  llvm::Value *derived =
      builder_.CreateLoad(ptr, builder_.CreateStructGEP(type, object, kReactionDerived), "derived");
  const Type *errorType = errorClass_->type;
  bool holds = promiseValueSize(result) != 0;
  llvm::FunctionCallee fulfillLater =
      asyncFunction("tz_promise_fulfill_later", builder_.getVoidTy(), {ptr});
  // The promise the code settles: that of the async function, which this
  // one follows once the code is done, or this one.
  llvm::Value *settled = derived;
  auto done = [&] {
    if (settled != derived)
      followPromise(derived, settled);
    builder_.CreateRetVoid();
  };

  auto callback = [&](unsigned field) {
    return builder_.CreateLoad(fnTy_, builder_.CreateStructGEP(type, object, field), "callback");
  };
  // Calls `fn` of `fnType` with the owned `arg` of `argType`, as a half of a
  // Result is given (undefined for void).
  auto callWith = [&](llvm::Value *fn, const Type *fnType, llvm::Value *arg, const Type *argType) {
    std::vector<llvm::Value *> args;
    if (fnType->params().empty()) {
      if (arg)
        releaseValue(arg, argType);
    } else {
      const Type *param = fnType->params()[0];
      const Type *given = argType->isVoid() ? Type::undefinedTy() : argType;
      llvm::Value *v = argType->isVoid() || !arg ? builder_.getFalse() : arg;
      args.push_back(given == param ? v : emitConversion(v, given, param));
    }
    return callFunctionValue(fn, fnType, args);
  };
  // After a callback of `fnType` that may throw: `onThrow` takes its error,
  // and the code is done.
  auto threw = [&](const Type *fnType, llvm::function_ref<void(llvm::Value *)> onThrow) {
    if (!typeThrows(fnType))
      return;
    llvm::Value *error = builder_.CreateLoad(ptr, thrownGlobal(), "thrown");
    llvm::BasicBlock *threwBlock = newBlock("callback.threw");
    llvm::BasicBlock *gaveBlock = newBlock("callback.gave");
    builder_.CreateCondBr(builder_.CreateIsNotNull(error), threwBlock, gaveBlock,
                          llvm::MDBuilder(ctx_).createUnlikelyBranchWeights());
    switchTo(threwBlock);
    builder_.CreateStore(llvm::ConstantPointerNull::get(ptr), thrownGlobal());
    onThrow(error);
    done();
    switchTo(gaveBlock);
  };
  auto reject = [&](llvm::Value *error) { rejectPromise(settled, error); };
  // A Result of `result`: an Ok (or an Err) of the owned half `v` of type
  // `from`.
  auto made = [&](bool ok, llvm::Value *v, const Type *from) {
    const Type *to = ok ? result->okType() : result->errType();
    llvm::Value *half = v && !from->isVoid() ? convertHalf(v, from, to) : nullptr;
    return makeResult(result, builder_.getInt1(ok), ok ? half : nullptr, ok ? nullptr : half);
  };
  // What a callback of andThen, orElse or match gave, of type `given`: a
  // promise to follow, or the value.
  auto settleWith = [&](llvm::Value *v, const Type *given) {
    if (given->isPromise() && given->promised() == result) {
      followPromise(settled, v);
    } else if (given->isPromise()) {
      // Its value converts on the way, which takes the turn of the queue
      // that following it would.
      attachReaction(
          v, settled,
          reactionHandler(Builtin::PromiseThen, given->promised(), result, nullptr, nullptr),
          nullptr, nullptr, /*later=*/true);
      releaseValue(v, given);
    } else {
      fulfillPromise(
          settled, holds && !given->isVoid() ? emitConversion(v, given, result) : nullptr, result);
    }
    done();
  };

  llvm::Value *state =
      builder_.CreateLoad(builder_.getInt32Ty(), promiseField(source, kState), "state");
  llvm::BasicBlock *fulfilledBlock = newBlock("fulfilled");
  llvm::BasicBlock *rejectedBlock = newBlock("rejected");
  builder_.CreateCondBr(builder_.CreateICmpEQ(state, builder_.getInt32(kFulfilled)), fulfilledBlock,
                        rejectedBlock);

  // Rejected: the error goes on, unless the code catches it.
  switchTo(rejectedBlock);
  llvm::Value *error = builder_.CreateLoad(ptr, promiseField(source, kError), "error");
  retainObject(error);
  switch (kind) {
  case Builtin::ResultAsyncFromPromise:
  case Builtin::ResultAsyncFromThrowable:
    // An Err of what toError makes of the error, or of the error.
    if (first) {
      llvm::Value *x = callWith(callback(kReactionFulfilled), first, error, errorType);
      threw(first, reject);
      fulfillPromise(derived, made(false, x, first->ret()), result);
    } else {
      fulfillPromise(derived, made(false, error, errorType), result);
    }
    break;
  case Builtin::ResultAsyncTeeDone:
    builder_.CreateCall(instanceReleaseFn_, {error}); // a tee does not care
    fulfillPromise(derived, nullptr, result);         // the Result is in the value already
    break;
  case Builtin::ResultAsyncUnwrapOr:
    if (holds) {
      // The default waits in the value: it goes.
      llvm::Value *slot = promiseValueAddress(derived);
      releaseValue(builder_.CreateLoad(lower(result), slot), result);
      builder_.CreateStore(llvm::Constant::getNullValue(lower(result)), slot);
    }
    rejectPromise(derived, error);
    break;
  default:
    rejectPromise(derived, error);
    break;
  }
  builder_.CreateRetVoid();

  // Fulfilled.
  switchTo(fulfilledBlock);
  llvm::Value *given = nullptr;
  if (promiseValueSize(value)) {
    given = builder_.CreateLoad(lower(value), promiseValueAddress(source), "value");
    retainValue(given, value);
  }
  switch (kind) {
  case Builtin::ResultAsyncFromPromise: // its catch(): the Result goes on
    fulfillPromise(derived, given, result);
    builder_.CreateRetVoid();
    break;
  case Builtin::ResultAsyncFromSafePromise:
  case Builtin::ResultAsyncFromThrowable:
  case Builtin::ResultAsyncWrapOk:
  case Builtin::ResultAsyncWrapErr:
    fulfillPromise(derived, made(kind != Builtin::ResultAsyncWrapErr, given, value), result);
    builder_.CreateRetVoid();
    break;
  case Builtin::ResultAsyncTeeDone:
    if (given)
      releaseValue(given, value);
    fulfillPromise(derived, nullptr, result); // the Result is in the value already
    builder_.CreateRetVoid();
    break;
  default: {
    // A Result came: the method goes by its half.
    if (kind == Builtin::ResultAsyncMap || kind == Builtin::ResultAsyncMapErr ||
        kind == Builtin::ResultAsyncOrElse || kind == Builtin::ResultAsyncAndTee ||
        kind == Builtin::ResultAsyncOrTee)
      settled = newPromise(result);
    const Type *okType = value->okType(), *errType = value->errType();
    llvm::Value *res = given ? given : llvm::Constant::getNullValue(lower(value));
    llvm::Value *okHalf = resultHalf(res, value, false), *errHalf = resultHalf(res, value, true);
    llvm::BasicBlock *okBlock = newBlock("ok");
    llvm::BasicBlock *errBlock = newBlock("err");
    builder_.CreateCondBr(resultIsOk(res, value), okBlock, errBlock);
    bool onError = kind == Builtin::ResultAsyncMapErr || kind == Builtin::ResultAsyncOrElse ||
                   kind == Builtin::ResultAsyncOrTee;
    bool tee = kind == Builtin::ResultAsyncAndTee || kind == Builtin::ResultAsyncOrTee;
    for (bool isOk : {true, false}) {
      switchTo(isOk ? okBlock : errBlock);
      llvm::Value *half = isOk ? okHalf : errHalf;
      const Type *halfType = isOk ? okType : errType;
      if (halfType->isNever()) { // a Result without this half
        builder_.CreateUnreachable();
        continue;
      }
      if (kind == Builtin::ResultAsyncUnwrapOr) {
        if (isOk && holds) {
          // The value instead of the default.
          llvm::Value *slot = promiseValueAddress(derived);
          releaseValue(builder_.CreateLoad(lower(result), slot), result);
          builder_.CreateStore(llvm::Constant::getNullValue(lower(result)), slot);
          retainValue(half, halfType);
          releaseValue(res, value);
          fulfillPromise(derived, emitConversion(half, halfType, result), result);
        } else {
          releaseValue(res, value);
          fulfillPromise(derived, nullptr, result); // the default
        }
        builder_.CreateRetVoid();
        continue;
      }
      if (kind != Builtin::ResultAsyncMatch && isOk == onError) {
        // The half nothing is called for goes on as it is.
        if (tee) {
          fulfillPromise(settled, res, result);
        } else {
          llvm::Value *keep = halfType->isVoid() ? nullptr : half;
          fulfillPromise(settled, made(isOk, keep, halfType), result);
        }
        done();
        continue;
      }
      bool onSecond = kind == Builtin::ResultAsyncMatch && !isOk;
      const Type *fnType = onSecond ? second : first;
      if (tee) {
        // The Result waits in the value until the callback is done.
        retainValue(half, halfType);
        if (holds)
          builder_.CreateStore(res, promiseValueAddress(settled));
        else
          releaseValue(res, value);
      }
      llvm::Value *x = callWith(callback(onSecond ? kReactionRejected : kReactionFulfilled), fnType,
                                half, halfType);
      const Type *ret = fnType->ret();
      switch (kind) {
      case Builtin::ResultAsyncMap:
      case Builtin::ResultAsyncMapErr:
        // `return ok(await f(value))`: once f's promise settles, or a turn
        // later.
        threw(fnType, reject);
        if (ret->isPromise()) {
          attachReaction(
              x, settled,
              resultAsyncHandler(isOk ? Builtin::ResultAsyncWrapOk : Builtin::ResultAsyncWrapErr,
                                 ret->promised(), result, nullptr, nullptr),
              nullptr, nullptr);
          releaseValue(x, ret);
        } else {
          if (holds)
            builder_.CreateStore(made(isOk, ret->isVoid() ? nullptr : x, ret),
                                 promiseValueAddress(settled));
          builder_.CreateCall(fulfillLater, {settled});
        }
        done();
        break;
      case Builtin::ResultAsyncAndTee:
      case Builtin::ResultAsyncOrTee:
        // `try { await f(value) } catch {}`, then the Result: when f
        // throws, the error goes and the Result goes on now.
        threw(fnType, [&](llvm::Value *thrown) {
          builder_.CreateCall(instanceReleaseFn_, {thrown});
          fulfillPromise(settled, nullptr, result);
        });
        if (ret->isPromise()) {
          attachReaction(x, settled,
                         resultAsyncHandler(Builtin::ResultAsyncTeeDone, ret->promised(), result,
                                            nullptr, nullptr),
                         nullptr, nullptr);
          releaseValue(x, ret);
        } else {
          if (x && !ret->isVoid())
            releaseValue(x, ret);
          builder_.CreateCall(fulfillLater, {settled});
        }
        done();
        break;
      default: // andThen, orElse and match
        threw(fnType, reject);
        settleWith(x, ret);
        break;
      }
    }
    break;
  }
  }
  function_ = outer;
  builder_.restoreIP(point);
  return handler;
}

llvm::Value *CodeGen::emitResultAsync(CallExpr *call) {
  if (call->builtin == Builtin::ResultAsyncFromThrowable)
    return emitResultAsyncThrowable(call);
  const Type *result = call->type->promised();
  bool fromPromise = call->builtin == Builtin::ResultAsyncFromPromise ||
                     call->builtin == Builtin::ResultAsyncFromSafePromise;
  Expr *sourceExpr = fromPromise ? call->args[0] : cast<MemberExpr>(call->callee)->object;
  std::vector<Expr *> exprs{sourceExpr};
  for (size_t i = fromPromise ? 1 : 0; i < call->args.size(); ++i)
    exprs.push_back(call->args[i]);
  std::vector<llvm::Value *> values = emitHeldValues(exprs);
  const Type *sourceType = sourceExpr->type;
  if (sourceType->isResult())
    return emitResultAsyncOf(call, values);
  llvm::Value *derived = newPromise(result);
  llvm::Value *callbacks[2] = {nullptr, nullptr};
  const Type *types[2] = {nullptr, nullptr};
  if (call->builtin == Builtin::ResultAsyncUnwrapOr) {
    // The default waits in the value of the promise until it is needed.
    if (promiseValueSize(result))
      builder_.CreateStore(values[1], promiseValueAddress(derived));
  } else {
    for (size_t i = 1; i < exprs.size() && i <= 2; ++i) {
      callbacks[i - 1] = values[i];
      types[i - 1] = exprs[i]->type;
    }
  }
  llvm::Value *source = values[0];
  if (call->builtin == Builtin::ResultAsyncFromPromise) {
    // `promise.then((v) => ok(v)).catch((e) => err(toError(e)))`.
    llvm::Value *step = newPromise(result);
    attachReaction(source, step,
                   resultAsyncHandler(Builtin::ResultAsyncFromSafePromise, sourceType->promised(),
                                      result, nullptr, nullptr),
                   nullptr, nullptr);
    attachReaction(
        step, derived,
        resultAsyncHandler(Builtin::ResultAsyncFromPromise, result, result, types[0], nullptr),
        callbacks[0], nullptr);
    builder_.CreateCall(instanceReleaseFn_, {step});
  } else {
    attachReaction(
        source, derived,
        resultAsyncHandler(call->builtin, sourceType->promised(), result, types[0], types[1]),
        callbacks[0], callbacks[1]);
  }
  releaseValue(source, sourceType);
  return derived;
}

// asyncMap and asyncAndThen of a Result, whose function runs now, as in
// neverthrow: an Ok gives `fromSafePromise(f(value))` and `f(value)`, an Err
// `errAsync(error)`.
llvm::Value *CodeGen::emitResultAsyncOf(CallExpr *call, llvm::ArrayRef<llvm::Value *> values) {
  const Type *result = call->type->promised();
  const Type *type = cast<MemberExpr>(call->callee)->object->type;
  const Type *fnType = call->args[0]->type, *ret = fnType->ret();
  llvm::Type *ptr = builder_.getPtrTy();
  llvm::Value *res = values[0], *fn = values[1];
  llvm::BasicBlock *okBlock = newBlock("ok");
  llvm::BasicBlock *errBlock = newBlock("err");
  llvm::BasicBlock *doneBlock = newBlock("done");
  builder_.CreateCondBr(resultIsOk(res, type), okBlock, errBlock);

  switchTo(errBlock);
  const Type *errType = type->errType();
  llvm::Value *failed = builder_.CreateCall(asyncFunction("tz_promise_resolved", ptr, {ptr}),
                                            {promiseLayout(result)}, "promise");
  llvm::Value *half = errType->isVoid() || errType->isNever()
                          ? nullptr
                          : convertHalf(resultHalf(res, type, true), errType, result->errType());
  builder_.CreateStore(makeResult(result, builder_.getFalse(), nullptr, half),
                       promiseValueAddress(failed));
  releaseValue(fn, fnType);
  llvm::BasicBlock *errEnd = builder_.GetInsertBlock();
  builder_.CreateBr(doneBlock);

  switchTo(okBlock);
  std::vector<llvm::Value *> args;
  const Type *okType = type->okType();
  llvm::Value *v = resultHalf(res, type, false);
  if (fnType->params().empty()) {
    if (!okType->isVoid() && !okType->isNever())
      releaseValue(v, okType);
  } else {
    const Type *param = fnType->params()[0];
    const Type *given = okType->isVoid() ? Type::undefinedTy() : okType;
    llvm::Value *arg = okType->isVoid() ? builder_.getFalse() : v;
    args.push_back(given == param ? arg : emitConversion(arg, given, param));
  }
  llvm::Value *gave = callFunctionValue(fn, fnType, args);
  releaseValue(fn, fnType);
  emitThrowCheckIf(typeThrows(fnType)); // it runs here, in the caller
  llvm::Value *made = gave;
  if (call->builtin == Builtin::ResultAsyncMap || ret->promised() != result) {
    made = newPromise(result);
    attachReaction(
        gave, made,
        call->builtin == Builtin::ResultAsyncMap
            ? resultAsyncHandler(Builtin::ResultAsyncFromSafePromise, ret->promised(), result,
                                 nullptr, nullptr)
            : reactionHandler(Builtin::PromiseThen, ret->promised(), result, nullptr, nullptr),
        nullptr, nullptr);
    releaseValue(gave, ret);
  }
  llvm::BasicBlock *okEnd = builder_.GetInsertBlock();
  builder_.CreateBr(doneBlock);

  switchTo(doneBlock);
  llvm::PHINode *promise = builder_.CreatePHI(ptr, 2, "result.async");
  promise->addIncoming(failed, errEnd);
  promise->addIncoming(made, okEnd);
  return promise;
}

// ResultAsync.fromThrowable(f, toError?): a function value whose
// environment holds f and toError. Its code is neverthrow's async function:
// it calls f and gives a ResultAsync of an Ok of what f's promise gives, or
// an Err of what f throws or the promise is rejected with, as toError makes
// it; once the promise settles, or now when f throws.
llvm::Value *CodeGen::emitResultAsyncThrowable(CallExpr *call) {
  const Type *fnType = call->args[0]->type;
  const Type *toErrorType = call->args.size() > 1 ? call->args[1]->type : nullptr;
  const Type *madeType = call->type;
  const Type *result = madeType->ret()->promised();
  std::vector<llvm::Value *> values = emitHeldValues(call->args);
  llvm::Type *ptr = builder_.getPtrTy(), *i64 = builder_.getInt64Ty();
  auto *envType = llvm::StructType::get(ctx_, {i64, ptr, fnTy_, fnTy_});
  auto *dropType = llvm::FunctionType::get(builder_.getVoidTy(), {ptr}, false);
  auto *drop = llvm::Function::Create(dropType, llvm::GlobalValue::InternalLinkage,
                                      "tz.fromAsyncThrowable.env.drop", module_.get());
  drop->addFnAttr(llvm::Attribute::NoUnwind);
  llvm::IRBuilderBase::InsertPoint point = builder_.saveIP();
  builder_.SetInsertPoint(llvm::BasicBlock::Create(ctx_, "entry", drop));
  for (unsigned i : {2u, 3u}) {
    llvm::Value *fn =
        builder_.CreateLoad(fnTy_, builder_.CreateStructGEP(envType, drop->getArg(0), i));
    builder_.CreateCall(objReleaseFn_, {builder_.CreateExtractValue(fn, 1)});
  }
  builder_.CreateCall(rt_.objFree, {drop->getArg(0)});
  builder_.CreateRetVoid();

  llvm::FunctionType *codeType = closureFnType(madeType);
  auto *code = llvm::Function::Create(codeType, llvm::GlobalValue::InternalLinkage,
                                      "tz.fromAsyncThrowable", module_.get());
  setFunctionAttributes(code);
  llvm::Function *outer = std::exchange(function_, code);
  builder_.SetInsertPoint(llvm::BasicBlock::Create(ctx_, "entry", code));
  llvm::Value *env = code->getArg(0);
  std::vector<llvm::Value *> args;
  for (unsigned i = 1; i < code->arg_size(); ++i)
    args.push_back(code->getArg(i));
  llvm::Value *f = builder_.CreateLoad(fnTy_, builder_.CreateStructGEP(envType, env, 2), "f");
  llvm::Value *toError =
      builder_.CreateLoad(fnTy_, builder_.CreateStructGEP(envType, env, 3), "toError");
  llvm::Value *promise = callFunctionValue(f, fnType, args);
  llvm::Value *derived = newPromise(result);
  if (typeThrows(fnType)) {
    // f threw rather than give a promise: an Err of that now.
    llvm::Value *thrown = builder_.CreateLoad(ptr, thrownGlobal(), "thrown");
    llvm::BasicBlock *threwBlock = newBlock("threw");
    llvm::BasicBlock *gaveBlock = newBlock("gave");
    builder_.CreateCondBr(builder_.CreateIsNotNull(thrown), threwBlock, gaveBlock,
                          llvm::MDBuilder(ctx_).createUnlikelyBranchWeights());
    switchTo(threwBlock);
    builder_.CreateStore(llvm::ConstantPointerNull::get(ptr), thrownGlobal());
    llvm::Value *errorHalf = thrown;
    const Type *errorType = errorClass_->type;
    if (toErrorType) {
      std::vector<llvm::Value *> errorArgs;
      if (!toErrorType->params().empty())
        errorArgs.push_back(thrown);
      else
        builder_.CreateCall(instanceReleaseFn_, {thrown});
      errorHalf = callFunctionValue(toError, toErrorType, errorArgs);
      errorType = toErrorType->ret();
      if (typeThrows(toErrorType)) {
        // toError threw in turn: that rejects the ResultAsync.
        llvm::Value *again = builder_.CreateLoad(ptr, thrownGlobal(), "thrown");
        llvm::BasicBlock *failedBlock = newBlock("failed");
        llvm::BasicBlock *madeBlock = newBlock("made");
        builder_.CreateCondBr(builder_.CreateIsNotNull(again), failedBlock, madeBlock,
                              llvm::MDBuilder(ctx_).createUnlikelyBranchWeights());
        switchTo(failedBlock);
        builder_.CreateStore(llvm::ConstantPointerNull::get(ptr), thrownGlobal());
        rejectPromise(derived, again);
        builder_.CreateRet(derived);
        switchTo(madeBlock);
      }
    }
    fulfillPromise(derived,
                   makeResult(result, builder_.getFalse(), nullptr,
                              convertHalf(errorHalf, errorType, result->errType())),
                   result);
    builder_.CreateRet(derived);
    switchTo(gaveBlock);
  }
  if (toErrorType)
    builder_.CreateCall(objRetainFn_, {builder_.CreateExtractValue(toError, 1)});
  attachReaction(promise, derived,
                 resultAsyncHandler(Builtin::ResultAsyncFromThrowable, fnType->ret()->promised(),
                                    result, toErrorType, nullptr),
                 toErrorType ? toError : nullptr, nullptr);
  builder_.CreateCall(instanceReleaseFn_, {promise});
  builder_.CreateRet(derived);
  function_ = outer;
  builder_.restoreIP(point);

  auto *info = new llvm::GlobalVariable(
      *module_, fnInfoTy_, /*isConstant=*/true, llvm::GlobalValue::PrivateLinkage,
      llvm::ConstantStruct::get(fnInfoTy_, {code, stringLiteral(u"")}),
      "tz.fninfo.fromAsyncThrowable");
  uint64_t size = module_->getDataLayout().getTypeAllocSize(envType);
  llvm::Value *object = builder_.CreateCall(rt_.objAlloc, {builder_.getInt64(size)}, "env");
  builder_.CreateStore(builder_.getInt64(1), builder_.CreateStructGEP(envType, object, 0));
  builder_.CreateStore(dropDescriptor(drop), builder_.CreateStructGEP(envType, object, 1));
  builder_.CreateStore(values[0], builder_.CreateStructGEP(envType, object, 2));
  builder_.CreateStore(values.size() > 1 ? values[1] : llvm::Constant::getNullValue(fnTy_),
                       builder_.CreateStructGEP(envType, object, 3));
  return makeFunctionValue(info, object);
}

//===----------------------------------------------------------------------===//
// Timers and the queues
//===----------------------------------------------------------------------===//

llvm::Value *CodeGen::emitAsyncBuiltin(CallExpr *call) {
  if (call->builtin >= Builtin::TimersSetTimeout)
    return emitAsyncIoBuiltin(call);
  llvm::Type *ptr = builder_.getPtrTy(), *i32 = builder_.getInt32Ty(),
             *f64 = builder_.getDoubleTy();
  llvm::Type *voidTy = builder_.getVoidTy();
  switch (call->builtin) {
  case Builtin::PromiseNew:
    return emitPromiseNew(call);
  case Builtin::PromiseThen:
  case Builtin::PromiseCatch:
  case Builtin::PromiseFinally:
    return emitPromiseReaction(call);
  case Builtin::ResultAsyncFromPromise:
  case Builtin::ResultAsyncFromSafePromise:
  case Builtin::ResultAsyncFromThrowable:
  case Builtin::ResultAsyncMap:
  case Builtin::ResultAsyncMapErr:
  case Builtin::ResultAsyncAndThen:
  case Builtin::ResultAsyncOrElse:
  case Builtin::ResultAsyncMatch:
  case Builtin::ResultAsyncUnwrapOr:
  case Builtin::ResultAsyncAndTee:
  case Builtin::ResultAsyncOrTee:
    return emitResultAsync(call);
  case Builtin::SetTimeout:
  case Builtin::SetInterval: {
    std::vector<llvm::Value *> args = emitHeldValues(call->args);
    llvm::Value *delay = args.size() > 1 ? args[1] : llvm::ConstantFP::get(f64, 1.0);
    auto *fn =
        cast<llvm::Function>(asyncFunction("tz_timer_new", ptr, {ptr, ptr, f64, i32}).getCallee());
    fn->addRetAttr(llvm::Attribute::NoAlias);
    return builder_.CreateCall(fn,
                               {builder_.CreateExtractValue(args[0], 0),
                                builder_.CreateExtractValue(args[0], 1), delay,
                                builder_.getInt32(call->builtin == Builtin::SetInterval)},
                               "timer");
  }
  case Builtin::SetImmediate:
  case Builtin::QueueMicrotask:
  case Builtin::NextTick: {
    llvm::Value *callback = emitExpr(call->args[0]);
    llvm::Value *info = builder_.CreateExtractValue(callback, 0);
    llvm::Value *env = builder_.CreateExtractValue(callback, 1);
    if (call->builtin == Builtin::SetImmediate)
      return builder_.CreateCall(asyncFunction("tz_immediate_new", ptr, {ptr, ptr}), {info, env},
                                 "immediate");
    builder_.CreateCall(
        asyncFunction(call->builtin == Builtin::NextTick ? "tz_next_tick" : "tz_queue_microtask",
                      voidTy, {ptr, ptr}),
        {info, env});
    return nullptr;
  }
  case Builtin::ClearTimeout:
  case Builtin::ClearImmediate: {
    Operand timer = emitOperand(call->args[0], /*canBorrow=*/true);
    llvm::Value *value = timer.value;
    if (call->args[0]->type->isUndefined())
      value = llvm::ConstantPointerNull::get(ptr);
    builder_.CreateCall(asyncFunction(call->builtin == Builtin::ClearTimeout ? "tz_timer_clear"
                                                                             : "tz_immediate_clear",
                                      voidTy, {ptr}),
                        {value});
    if (!call->args[0]->type->isUndefined())
      dropOperand(timer);
    return nullptr;
  }
  case Builtin::TimerRef:
  case Builtin::TimerUnref:
  case Builtin::TimerRefresh: {
    // The timer itself.
    llvm::Value *timer = emitExpr(cast<MemberExpr>(call->callee)->object);
    if (call->builtin == Builtin::TimerRefresh)
      builder_.CreateCall(asyncFunction("tz_timer_refresh", voidTy, {ptr}), {timer});
    else
      builder_.CreateCall(asyncFunction("tz_timer_ref", voidTy, {ptr, i32}),
                          {timer, builder_.getInt32(call->builtin == Builtin::TimerRef)});
    return timer;
  }
  case Builtin::TimerHasRef: {
    Operand timer = emitOperand(cast<MemberExpr>(call->callee)->object, /*canBorrow=*/true);
    llvm::Value *has =
        builder_.CreateCall(asyncFunction("tz_timer_has_ref", i32, {ptr}), {timer.value});
    dropOperand(timer);
    return builder_.CreateIsNotNull(has);
  }
  default:
    return emitPromiseStatic(call);
  }
}

} // namespace tz
