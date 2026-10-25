// Generators and iterators (docs/iterators.md).
//
// An iterator is an object of the runtime: { i64 rc, ptr layout }, and then
// the frame of an LLVM coroutine (the switch-resumed kind) that gives its
// values. The promise of the coroutine, right after the two function
// pointers that start every frame, is { ptr out, ptr in, ptr ret, i32
// running, i32 close }: before resuming it, next() points `out` at a slot
// for the value it wants, `in` at what it sends (or null) and `ret` at a slot
// for what a generator returns (or null). The drop function of the layout
// destroys the coroutine, which releases what its frame holds where it
// waits, and frees it.
//
// A generator that waits at a `yield` inside a `try` with a `finally` sets
// `close` to 1: the iterator is dropped before its end (a `for...of` left by
// `break`, `return` or an error, a helper that stops early), and as
// return() in JavaScript, the drop resumes it with `close` 2 first, which
// makes that `yield` a `return`: the `finally` blocks run.
//
// Generator functions are such coroutines; so are the iterators of arrays
// and of the helpers (map, filter...) when they are made as values. Where the
// values are consumed right where the iterator is made (`for...of`, a
// spread, toArray...), no object is made: prepare() evaluates the operands of
// the chain, and its run() emits one loop that hands each value to the
// consumer. A generator called there is emitted inline: its body in place,
// with the consumer at each `yield`.

#include "compiler/lower/codegen/codegen.h"
#include "compiler/check/sema/sema.h"

#include "llvm/ADT/STLExtras.h"
#include "llvm/IR/MDBuilder.h"
#include "llvm/Support/ErrorHandling.h"

using llvm::cast;
using llvm::dyn_cast;
using llvm::isa;

namespace tz {

namespace {

// What an iterator prints as; must match TzLayout in runtime/core/tzrt/tzrt.h.
enum : unsigned {
  kLayoutGenerator = 2,      // Object [Generator] {}
  kLayoutArrayIterator = 4,  // Object [Array Iterator] {}
  kLayoutStringIterator = 8, // Object [String Iterator] {}
  kLayoutHelper = 16,        // Object [Iterator Helper] {}
  kLayoutRegexIterator = 256, // Object [RegExp String Iterator] {}
  kLayoutDictIterator = 8192, // [Map Iterator] { 'a' }, with what follows
  kLayoutDictValues = 16384,
  kLayoutDictEntries = 32768,
  kLayoutDictOfSet = 65536,
};

// The header before the frame of the coroutine: { i64 rc, ptr layout }.
constexpr uint64_t kHeaderSize = 16;
// The alignment of the frame, which follows the header of an allocation.
constexpr uint32_t kFrameAlign = 8;
enum PromiseField : unsigned { kOut, kIn, kRet, kRunning, kClose };

// A generator with more `yield`s than this is not emitted inline: the
// consumer is copied to each of them.
constexpr unsigned kMaxInlineYields = 8;
// Nor one emitted inline in so many others, or where the code would be
// copied more times than this: generators emitted inline in the consumers of
// others multiply their copies.
constexpr unsigned kMaxInlineDepth = 4;
constexpr unsigned kMaxInlineCopies = 16;

// What iterating a value of `type` gives (see Sema::iteratedType).
const Type *iteratedType(const Type *type) {
  if (type->isTypedArray())
    return type->elementType();
  return type->isArray() ? type->inner() : type->isString() ? type : type->yielded();
}

// Whether `stmt` has a `try`, outside the functions in it.
bool hasTry(const Stmt *stmt) {
  bool found = false;
  std::function<void(const Stmt *)> visit = [&](const Stmt *s) {
    if (!s || found)
      return;
    switch (s->kind()) {
    case Stmt::Kind::Try:
      found = true;
      break;
    case Stmt::Kind::Block:
      for (const Stmt *inner : cast<BlockStmt>(s)->body)
        visit(inner);
      break;
    case Stmt::Kind::If:
      visit(cast<IfStmt>(s)->thenStmt);
      visit(cast<IfStmt>(s)->elseStmt);
      break;
    case Stmt::Kind::While:
      visit(cast<WhileStmt>(s)->body);
      break;
    case Stmt::Kind::DoWhile:
      visit(cast<DoWhileStmt>(s)->body);
      break;
    case Stmt::Kind::For:
      visit(cast<ForStmt>(s)->body);
      break;
    case Stmt::Kind::ForOf:
      visit(cast<ForOfStmt>(s)->body);
      break;
    case Stmt::Kind::Switch:
      for (const SwitchCase &c : cast<SwitchStmt>(s)->cases)
        for (const Stmt *inner : c.body)
          visit(inner);
      break;
    case Stmt::Kind::Labeled:
      visit(cast<LabeledStmt>(s)->body);
      break;
    default:
      break;
    }
  };
  visit(stmt);
  return found;
}

unsigned countYields(const FunctionDecl *fn) {
  unsigned count = 0;
  visitExprs(
      fn->body, [&](const Expr *e) { count += isa<YieldExpr>(e) ? 1 : 0; },
      /*intoFunctions=*/false);
  return count;
}

} // namespace

llvm::Function *CodeGen::coroIntrinsic(llvm::Intrinsic::ID id, llvm::ArrayRef<llvm::Type *> types) {
  return llvm::Intrinsic::getOrInsertDeclaration(module_.get(), id, types);
}

CodeGen::EmitContext CodeGen::saveContext() {
  return {currentFn_, envParam_, returnType_, tailGroup_, targets_, inline_};
}

void CodeGen::restoreContext(const EmitContext &context) {
  currentFn_ = context.fn;
  envParam_ = context.env;
  returnType_ = context.returnType;
  tailGroup_ = context.tailGroup;
  targets_ = context.targets;
  inline_ = context.inlined;
}

//===----------------------------------------------------------------------===//
// Coroutines
//===----------------------------------------------------------------------===//

llvm::Constant *CodeGen::iteratorLayout(unsigned flags) {
  llvm::Constant *&layout = iteratorLayouts_[flags];
  if (layout)
    return layout;
  if (!iterDropFn_) {
    auto *type = llvm::FunctionType::get(builder_.getVoidTy(), {builder_.getPtrTy()}, false);
    iterDropFn_ = llvm::Function::Create(type, llvm::GlobalValue::InternalLinkage,
                                         "tz.iter.destroy", module_.get());
    iterDropFn_->addFnAttr(llvm::Attribute::NoUnwind);
    llvm::IRBuilder<> b(llvm::BasicBlock::Create(ctx_, "entry", iterDropFn_));
    // Closed first, if it waits at a `yield` with a `finally` around it.
    b.CreateCall(iterCloseFunction(), {iterDropFn_->getArg(0)});
    llvm::Value *frame =
        b.CreateConstInBoundsGEP1_64(b.getInt8Ty(), iterDropFn_->getArg(0), kHeaderSize, "frame");
    b.CreateCall(coroIntrinsic(llvm::Intrinsic::coro_destroy), {frame});
    b.CreateRetVoid();
  }
  auto *global = new llvm::GlobalVariable(
      *module_, layoutDescTy_, /*isConstant=*/true, llvm::GlobalValue::PrivateLinkage,
      llvm::ConstantStruct::get(layoutDescTy_,
                                {iterDropFn_, llvm::ConstantPointerNull::get(builder_.getPtrTy()),
                                 builder_.getInt32(0), builder_.getInt32(flags),
                                 // not an instance of a class
                                 llvm::ConstantPointerNull::get(builder_.getPtrTy()),
                                 builder_.getInt32(0), builder_.getInt32(0)}),
      "tz.layout.iterator");
  global->setUnnamedAddr(llvm::GlobalValue::UnnamedAddr::Global);
  layout = global;
  return layout;
}

// return() of JavaScript, as `for...of` and the iterator helpers call it when
// they stop before the end, and as dropping an iterator does: a generator
// that waits at a `yield` with a `finally` around it is resumed to return
// from there, which runs them; any other iterator is left as it is. The
// error in flight waits meanwhile; one the `finally` blocks throw is dropped,
// as nothing could catch it where an iterator is closed.
llvm::Function *CodeGen::iterCloseFunction() {
  if (iterCloseFn_)
    return iterCloseFn_;
  auto *type = llvm::FunctionType::get(builder_.getVoidTy(), {builder_.getPtrTy()}, false);
  iterCloseFn_ = llvm::Function::Create(type, llvm::GlobalValue::InternalLinkage, "tz.iter.close",
                                        module_.get());
  iterCloseFn_->addFnAttr(llvm::Attribute::NoUnwind);
  llvm::BasicBlock *entry = llvm::BasicBlock::Create(ctx_, "entry", iterCloseFn_);
  llvm::BasicBlock *checkBlock = llvm::BasicBlock::Create(ctx_, "waits", iterCloseFn_);
  llvm::BasicBlock *closeBlock = llvm::BasicBlock::Create(ctx_, "close", iterCloseFn_);
  llvm::BasicBlock *failedBlock = llvm::BasicBlock::Create(ctx_, "close.failed", iterCloseFn_);
  llvm::BasicBlock *closedBlock = llvm::BasicBlock::Create(ctx_, "closed", iterCloseFn_);
  llvm::BasicBlock *endBlock = llvm::BasicBlock::Create(ctx_, "end", iterCloseFn_);
  llvm::IRBuilder<> b(entry);
  llvm::Type *ptr = b.getPtrTy(), *i32 = b.getInt32Ty();
  llvm::Value *frame =
      b.CreateConstInBoundsGEP1_64(b.getInt8Ty(), iterCloseFn_->getArg(0), kHeaderSize, "frame");
  b.CreateCondBr(b.CreateCall(coroIntrinsic(llvm::Intrinsic::coro_done), {frame}, "done"),
                 endBlock, checkBlock);
  b.SetInsertPoint(checkBlock);
  llvm::Value *promise = b.CreateCall(coroIntrinsic(llvm::Intrinsic::coro_promise),
                                      {frame, b.getInt32(kFrameAlign), b.getFalse()}, "promise");
  llvm::Value *close = b.CreateStructGEP(promiseTy_, promise, kClose);
  b.CreateCondBr(b.CreateICmpEQ(b.CreateLoad(i32, close), b.getInt32(1)), closeBlock, endBlock);
  b.SetInsertPoint(closeBlock);
  llvm::Value *saved = b.CreateLoad(ptr, thrownSlot(b), "saved");
  llvm::Value *null = llvm::ConstantPointerNull::get(b.getPtrTy());
  b.CreateStore(null, thrownSlot(b));
  b.CreateStore(b.getInt32(2), close);
  for (unsigned field : {kOut, kIn, kRet})
    b.CreateStore(null, b.CreateStructGEP(promiseTy_, promise, field));
  llvm::Value *running = b.CreateStructGEP(promiseTy_, promise, kRunning);
  b.CreateStore(b.getInt32(1), running);
  b.CreateCall(coroIntrinsic(llvm::Intrinsic::coro_resume), {frame});
  b.CreateStore(b.getInt32(0), running);
  // A `yield` in a `finally` stopped it: it stays where it is, as in
  // JavaScript, until it is dropped.
  b.CreateStore(b.getInt32(0), close);
  llvm::Value *error = b.CreateLoad(ptr, thrownSlot(b), "error");
  b.CreateCondBr(b.CreateIsNotNull(error), failedBlock, closedBlock);
  b.SetInsertPoint(failedBlock);
  b.CreateCall(objReleaseFn_, {error});
  b.CreateBr(closedBlock);
  b.SetInsertPoint(closedBlock);
  b.CreateStore(saved, thrownSlot(b));
  b.CreateBr(endBlock);
  b.SetInsertPoint(endBlock);
  b.CreateRetVoid();
  return iterCloseFn_;
}

// The start of a coroutine: its frame is allocated after a header that makes
// it an object of the runtime, which the ramp returns.
void CodeGen::beginCoroutine(unsigned layoutFlags, const Type *sent, llvm::StructType *promise) {
  function_->addFnAttr(llvm::Attribute::PresplitCoroutine);
  Coroutine &c = *coro_;
  llvm::Type *i8 = builder_.getInt8Ty();
  llvm::Value *null = llvm::ConstantPointerNull::get(builder_.getPtrTy());
  c.sent = sent;
  c.promise = createStackSlot(promise ? promise : promiseTy_, "promise");
  c.id = builder_.CreateCall(coroIntrinsic(llvm::Intrinsic::coro_id),
                             {builder_.getInt32(kFrameAlign), c.promise, null, null}, "id");
  llvm::Value *size = builder_.CreateCall(
      coroIntrinsic(llvm::Intrinsic::coro_size, {builder_.getInt64Ty()}), {}, "size");
  llvm::Value *memory = builder_.CreateCall(
      rt_.objAlloc, {builder_.CreateAdd(size, builder_.getInt64(kHeaderSize))}, "memory");
  builder_.CreateStore(builder_.getInt64(1), memory);
  builder_.CreateStore(iteratorLayout(layoutFlags),
                       builder_.CreateConstInBoundsGEP1_64(i8, memory, 8));
  llvm::Value *frame = builder_.CreateConstInBoundsGEP1_64(i8, memory, kHeaderSize, "frame");
  c.handle =
      builder_.CreateCall(coroIntrinsic(llvm::Intrinsic::coro_begin), {c.id, frame}, "handle");
  // The object the ramp returns comes from the handle: LLVM takes the frame
  // to be reached only through it.
  c.object = builder_.CreateGEP(
      i8, c.handle, llvm::ConstantInt::getSigned(builder_.getInt64Ty(), -int64_t(kHeaderSize)),
      "iterator");
  builder_.CreateStore(llvm::Constant::getNullValue(promise ? promise : promiseTy_), c.promise);
  c.finalBlock = newBlock("coro.final");
  c.freeBlock = newBlock("coro.free");
  c.returnBlock = newBlock("coro.return");
}

void CodeGen::emitSuspend() {
  Coroutine &c = *coro_;
  llvm::Value *state =
      builder_.CreateCall(coroIntrinsic(llvm::Intrinsic::coro_suspend),
                          {llvm::ConstantTokenNone::get(ctx_), builder_.getFalse()}, "suspend");
  llvm::BasicBlock *resumeBlock = newBlock("coro.resume");
  llvm::BasicBlock *destroyBlock = newBlock("coro.destroy");
  llvm::SwitchInst *next = builder_.CreateSwitch(state, c.returnBlock, 2);
  next->addCase(builder_.getInt8(0), resumeBlock);
  next->addCase(builder_.getInt8(1), destroyBlock);
  // Destroyed while it waits here: what its scopes hold is released.
  switchTo(destroyBlock);
  bool noAwait = std::exchange(cleanupNoAwait_, true);
  emitCleanups(0);
  cleanupNoAwait_ = noAwait;
  builder_.CreateBr(c.freeBlock);
  switchTo(resumeBlock);
}

llvm::Value *CodeGen::emitCoroutineYield(llvm::Value *value, const Type *resultType) {
  Coroutine &c = *coro_;
  if (!c.closes) {
    llvm::Value *out = builder_.CreateLoad(
        builder_.getPtrTy(), builder_.CreateStructGEP(promiseTy_, c.promise, kOut), "out");
    builder_.CreateStore(value, out);
    emitSuspend();
  } else {
    // Whether a `finally` waits here, which closing the generator runs; and
    // while it closes, a `yield` in a `finally` gives nothing: the generator
    // stops there, as no one asks for more.
    bool closable = finallyBetween(~size_t(0)) != nullptr;
    llvm::Value *close = builder_.CreateStructGEP(promiseTy_, c.promise, kClose);
    llvm::BasicBlock *giveBlock = newBlock("yield.give");
    llvm::BasicBlock *dropBlock = newBlock("yield.closing");
    llvm::BasicBlock *suspendBlock = newBlock("yield.suspend");
    builder_.CreateCondBr(
        builder_.CreateICmpEQ(builder_.CreateLoad(builder_.getInt32Ty(), close), builder_.getInt32(2)),
        dropBlock, giveBlock);
    switchTo(giveBlock);
    llvm::Value *out = builder_.CreateLoad(
        builder_.getPtrTy(), builder_.CreateStructGEP(promiseTy_, c.promise, kOut), "out");
    builder_.CreateStore(value, out);
    builder_.CreateStore(builder_.getInt32(closable ? 1 : 0), close);
    builder_.CreateBr(suspendBlock);
    switchTo(dropBlock);
    releaseValue(value, c.yielded);
    builder_.CreateBr(suspendBlock);
    switchTo(suspendBlock);
    emitSuspend();
    if (closable) {
      // Resumed to close: a `return` from here.
      llvm::BasicBlock *closeBlock = newBlock("yield.close");
      llvm::BasicBlock *goBlock = newBlock("yield.resumed");
      builder_.CreateCondBr(builder_.CreateICmpEQ(builder_.CreateLoad(builder_.getInt32Ty(), close),
                                                  builder_.getInt32(2)),
                            closeBlock, goBlock, llvm::MDBuilder(ctx_).createUnlikelyBranchWeights());
      switchTo(closeBlock);
      emitReturnOf(nullptr);
      if (blockIsOpen())
        builder_.CreateUnreachable();
      switchTo(goBlock);
    }
  }
  if (!resultType)
    return nullptr;
  if (resultType->isUndefined())
    return builder_.getFalse();
  if (!c.sent)
    return undefinedValue(resultType);
  // What next(x) sent, which the caller keeps: a copy for the generator.
  llvm::Value *in = builder_.CreateLoad(builder_.getPtrTy(),
                                        builder_.CreateStructGEP(promiseTy_, c.promise, kIn), "in");
  llvm::BasicBlock *before = builder_.GetInsertBlock();
  llvm::BasicBlock *givenBlock = newBlock("yield.sent");
  llvm::BasicBlock *endBlock = newBlock("yield.end");
  llvm::BasicBlock *noneBlock = endBlock;
  if (!resultType->isOptional())
    noneBlock = newBlock("yield.none");
  builder_.CreateCondBr(builder_.CreateIsNotNull(in), givenBlock, noneBlock);
  if (!resultType->isOptional()) {
    // next() without a value resumed a `yield` whose value is needed.
    switchTo(noneBlock);
    emitThrowNew(1, "next() resumed a 'yield' that needs a value of type '" +
              resultType->name().str() + "' without one");
  }
  switchTo(givenBlock);
  llvm::Value *sent = builder_.CreateLoad(lower(c.sent), in, "sent");
  retainValue(sent, c.sent);
  sent = emitConversion(sent, c.sent, resultType);
  llvm::BasicBlock *givenEnd = builder_.GetInsertBlock();
  builder_.CreateBr(endBlock);
  switchTo(endBlock);
  llvm::PHINode *result = builder_.CreatePHI(lower(resultType), 2);
  result->addIncoming(sent, givenEnd);
  if (resultType->isOptional())
    result->addIncoming(undefinedValue(resultType), before);
  return result;
}

// The coroutine is done: what it returns goes where the caller wants it, if
// anywhere, and it waits at its final suspension to be destroyed.
void CodeGen::emitCoroutineReturn(llvm::Value *value, const Type *type) {
  Coroutine &c = *coro_;
  if (value && type && !type->isVoid() && !type->isUndefined()) {
    llvm::Value *ret = builder_.CreateLoad(
        builder_.getPtrTy(), builder_.CreateStructGEP(promiseTy_, c.promise, kRet), "ret");
    llvm::BasicBlock *storeBlock = newBlock("return.store");
    llvm::BasicBlock *dropBlock = newBlock("return.drop");
    llvm::BasicBlock *doneBlock = newBlock("return.done");
    builder_.CreateCondBr(builder_.CreateIsNotNull(ret), storeBlock, dropBlock);
    switchTo(storeBlock);
    builder_.CreateStore(value, ret);
    builder_.CreateBr(doneBlock);
    switchTo(dropBlock);
    releaseValue(value, type);
    builder_.CreateBr(doneBlock);
    switchTo(doneBlock);
  }
  emitCleanups(0);
  if (async_ && async_->generator)
    finishAsyncGenerator(nullptr);
  builder_.CreateBr(c.finalBlock);
  startDeadBlock();
}

void CodeGen::finishCoroutine() {
  Coroutine &c = *coro_;
  if (blockIsOpen())
    builder_.CreateUnreachable();
  llvm::Value *none = llvm::ConstantTokenNone::get(ctx_);
  // The final suspension; next() never resumes it from there.
  switchTo(c.finalBlock);
  llvm::Value *state = builder_.CreateCall(coroIntrinsic(llvm::Intrinsic::coro_suspend),
                                           {none, builder_.getTrue()}, "final");
  llvm::BasicBlock *resumedBlock = newBlock("coro.resumed.done");
  llvm::SwitchInst *next = builder_.CreateSwitch(state, c.returnBlock, 2);
  next->addCase(builder_.getInt8(0), resumedBlock);
  next->addCase(builder_.getInt8(1), c.freeBlock);
  switchTo(resumedBlock);
  builder_.CreateUnreachable();

  // Freeing: the frame starts after the header of the object.
  switchTo(c.freeBlock);
  llvm::Value *frame =
      builder_.CreateCall(coroIntrinsic(llvm::Intrinsic::coro_free), {c.id, c.handle}, "mem");
  llvm::BasicBlock *freeObject = newBlock("coro.free.object");
  builder_.CreateCondBr(builder_.CreateIsNotNull(frame), freeObject, c.returnBlock);
  switchTo(freeObject);
  llvm::Value *back = llvm::ConstantInt::getSigned(builder_.getInt64Ty(), -int64_t(c.header));
  builder_.CreateCall(rt_.objFree, {builder_.CreateGEP(builder_.getInt8Ty(), frame, back)});
  builder_.CreateBr(c.returnBlock);

  switchTo(c.returnBlock);
  builder_.CreateCall(coroIntrinsic(llvm::Intrinsic::coro_end),
                      {c.handle, builder_.getFalse(), none});
  builder_.CreateRet(c.object);
}

// A generator function as a coroutine: a call binds the parameters and
// returns the generator, which runs the body up to the first `yield` when
// next() is first called.
void CodeGen::emitGenerator(FunctionDecl *fn, llvm::Function *function) {
  if (fn->isAsync) {
    emitAsyncGenerator(fn, function);
    return;
  }
  beginFunction(function, fn);
  Coroutine coroutine{};
  coro_ = &coroutine;
  const Type *type = fn->sym->ret;
  beginCoroutine(kLayoutGenerator,
                 type->isIterator() && !type->sent()->isVoid() ? type->sent() : nullptr);
  coroutine.closes = hasTry(fn->body);
  coroutine.yielded = type->isIterator() ? type->yielded() : nullptr;
  pushScope(); // the parameters, and the environment of a closure
  std::vector<llvm::Value *> args;
  unsigned first = 0;
  if (fn->isClosure() || fn->takesThis()) {
    envParam_ = function_->getArg(0);
    if (fn->isClosure())
      bindCaptures(fn);
    else
      envParam_->setName("this");
    // The function value that made the generator may go before it does, and
    // so may the object of a method, which a call only lends.
    llvm::Value *slot = createObjectSlot("env");
    builder_.CreateCall(objRetainFn_, {envParam_});
    builder_.CreateStore(envParam_, slot);
    first = 1;
  }
  for (unsigned i = first; i < function_->arg_size(); ++i)
    args.push_back(function_->getArg(i));
  emitParams(fn, args);
  emitSuspend();
  emitStmt(fn->body);
  if (blockIsOpen())
    emitCoroutineReturn(nullptr, nullptr);
  finishScopeUnwind(0);
  scopes_.pop_back(); // every way out released it
  finishFunctionUnwind();
  finishCoroutine();
  coro_ = nullptr;
  function_ = nullptr;
}

llvm::Function *CodeGen::iteratorFunction(Builtin builtin, const Type *source, const Type *operand,
                                          const Type *result) {
  llvm::Function *&fn = iteratorFunctions_[{builtin, source, operand, result}];
  if (fn)
    return fn;
  std::vector<llvm::Type *> params{lower(source)};
  if (operand)
    params.push_back(lower(operand));
  if (builtin == Builtin::StrMatchAll)
    params.push_back(builder_.getPtrTy()); // the shape of the matches
  const char *name = "tz.iter";
  switch (builtin) {
  case Builtin::ArrKeys: name = "tz.iter.keys"; break;
  case Builtin::ArrValues: name = "tz.iter.values"; break;
  case Builtin::ArrEntries: name = "tz.iter.entries"; break;
  case Builtin::IterFrom: name = "tz.iter.string"; break;
  case Builtin::IterMap: name = "tz.iter.map"; break;
  case Builtin::IterFilter: name = "tz.iter.filter"; break;
  case Builtin::IterTake: name = "tz.iter.take"; break;
  case Builtin::IterDrop: name = "tz.iter.drop"; break;
  case Builtin::IterFlatMap: name = "tz.iter.flatMap"; break;
  case Builtin::StrMatchAll: name = "tz.iter.matchAll"; break;
  case Builtin::MapKeys: name = "tz.iter.mapKeys"; break;
  case Builtin::MapValues: name = "tz.iter.mapValues"; break;
  case Builtin::MapEntries: name = "tz.iter.mapEntries"; break;
  case Builtin::SetValues: name = "tz.iter.setValues"; break;
  case Builtin::SetEntries: name = "tz.iter.setEntries"; break;
  default: break;
  }
  fn = llvm::Function::Create(llvm::FunctionType::get(builder_.getPtrTy(), params, false),
                              llvm::GlobalValue::InternalLinkage, name, module_.get());
  setFunctionAttributes(fn);
  pendingIterators_.push_back({fn, builtin, source, operand, result});
  return fn;
}

// The iterator of an array or a string, or of a helper over another
// iterator, as an object: a coroutine that holds its operands.
void CodeGen::emitPendingIterators() {
  while (!pendingIterators_.empty()) {
    IteratorJob job = pendingIterators_.back();
    pendingIterators_.pop_back();
    beginFunction(job.function, nullptr);
    Coroutine coroutine{};
    coro_ = &coroutine;
    bool stage = isIteratorStage(job.builtin);
    bool matches = job.builtin == Builtin::StrMatchAll;
    bool dict = isDictIterator(job.builtin);
    // The iterator of a dictionary keeps the map and its position in the
    // promise too, where printing finds them: { ..., ptr map, i32 position }.
    llvm::StructType *dictPromise = nullptr;
    unsigned flags = 0;
    if (dict) {
      llvm::Type *ptr = builder_.getPtrTy(), *i32 = builder_.getInt32Ty();
      dictPromise = llvm::StructType::get(ctx_, {ptr, ptr, ptr, i32, i32, ptr, i32, i32});
      flags = kLayoutDictIterator |
              (job.builtin == Builtin::MapValues ? kLayoutDictValues : 0) |
              (job.builtin == Builtin::MapEntries || job.builtin == Builtin::SetEntries
                   ? kLayoutDictEntries
                   : 0) |
              (job.source->isSet() ? kLayoutDictOfSet : 0);
    }
    beginCoroutine(stage                    ? kLayoutHelper
                   : matches                ? kLayoutRegexIterator
                   : dict                   ? flags
                   : job.source->isString() ? kLayoutStringIterator
                                            : kLayoutArrayIterator,
                   nullptr, dictPromise);
    pushScope();
    auto hold = [&](llvm::Value *value, const Type *type, const char *name) {
      llvm::Value *slot =
          type->isManaged() ? createManagedSlot(type, name) : createStackSlot(lower(type), name);
      builder_.CreateStore(value, slot);
      return slot;
    };
    llvm::Value *source = hold(function_->getArg(0), job.source, "source");
    llvm::Value *operand =
        job.operand ? hold(function_->getArg(1), job.operand, "operand") : nullptr;
    // `s.matchAll(re)`: the string and the regex, whose g flag was checked;
    // the lastIndex of the copy is taken when the iterator is made.
    llvm::Value *lastSlot = nullptr;
    if (matches) {
      lastSlot = createStackSlot(builder_.getInt32Ty(), "lastIndex");
      llvm::Value *regex = builder_.CreateLoad(builder_.getPtrTy(), operand, "regex");
      builder_.CreateStore(
          builder_.CreateCall(regexFunction("tz_re_match_all_begin", builder_.getInt32Ty(),
                                            {builder_.getPtrTy()}),
                              {regex}),
          lastSlot);
    }
    // The map of a dictionary's iterator is there to be printed before the
    // iterator first runs.
    llvm::Value *mapField = nullptr;
    if (dict) {
      mapField = builder_.CreateStructGEP(dictPromise, coroutine.promise, 5);
      builder_.CreateStore(builder_.CreateLoad(builder_.getPtrTy(), source), mapField);
    }
    emitSuspend();
    Consumer yield;
    yield.each = [&](llvm::Value *value) { return emitCoroutineYield(value, nullptr); };
    yield.stop = [&] {
      emitCoroutineReturn(nullptr, nullptr);
      builder_.CreateUnreachable();
    };
    if (matches) {
      matchAllProducer(source, operand, lastSlot, function_->getArg(2)).run(yield);
    } else if (dict) {
      // The entries stay where they are while the iterator lives, so that
      // its position stays good, and the map is read from the promise.
      beginDictIteration(source);
      llvm::Value *position = builder_.CreateStructGEP(dictPromise, coroutine.promise, 6);
      dictProducer(mapField, job.source, job.builtin, position).run(yield);
      if (blockIsOpen()) // done: nothing is left to show
        builder_.CreateStore(llvm::ConstantPointerNull::get(builder_.getPtrTy()), mapField);
    } else if (!stage) {
      Producer producer = job.source->isArray() ? arrayProducer(source, job.source, job.builtin)
                                                : valueProducer(source, job.source);
      producer.run(yield);
    } else {
      // The stage before it is the iterator it holds.
      const Type *callback = job.operand->isFunction() ? job.operand : nullptr;
      valueProducer(source, job.source)
          .run(stageConsumer(job.builtin, callback, job.source->yielded(), job.result->yielded(),
                             operand, yield));
    }
    if (blockIsOpen())
      emitCoroutineReturn(nullptr, nullptr);
    finishScopeUnwind(0);
    scopes_.pop_back();
    finishFunctionUnwind();
    finishCoroutine();
    coro_ = nullptr;
    function_ = nullptr;
  }
}

//===----------------------------------------------------------------------===//
// Iterating
//===----------------------------------------------------------------------===//

llvm::Value *CodeGen::emitResume(llvm::Value *it, llvm::Value *out, llvm::Value *in,
                                 llvm::Value *ret) {
  llvm::Type *ptr = builder_.getPtrTy();
  llvm::Value *frame =
      builder_.CreateConstInBoundsGEP1_64(builder_.getInt8Ty(), it, kHeaderSize, "frame");
  llvm::BasicBlock *before = builder_.GetInsertBlock();
  llvm::BasicBlock *resumeBlock = newBlock("next.resume");
  llvm::BasicBlock *endBlock = newBlock("next.end");
  llvm::Value *done =
      builder_.CreateCall(coroIntrinsic(llvm::Intrinsic::coro_done), {frame}, "done");
  builder_.CreateCondBr(done, endBlock, resumeBlock);

  switchTo(resumeBlock);
  llvm::Value *promise =
      builder_.CreateCall(coroIntrinsic(llvm::Intrinsic::coro_promise),
                          {frame, builder_.getInt32(kFrameAlign), builder_.getFalse()}, "promise");
  // A generator that calls next() on itself, as JavaScript reports.
  llvm::Value *running = builder_.CreateStructGEP(promiseTy_, promise, kRunning);
  llvm::BasicBlock *runningBlock = newBlock("next.running");
  llvm::BasicBlock *goBlock = newBlock("next.go");
  builder_.CreateCondBr(builder_.CreateICmpNE(builder_.CreateLoad(builder_.getInt32Ty(), running),
                                              builder_.getInt32(0)),
                        runningBlock, goBlock, llvm::MDBuilder(ctx_).createUnlikelyBranchWeights());
  switchTo(runningBlock);
  emitThrowNew(1, "Generator is already running");
  switchTo(goBlock);
  llvm::Value *null = llvm::ConstantPointerNull::get(ptr);
  builder_.CreateStore(out, builder_.CreateStructGEP(promiseTy_, promise, kOut));
  builder_.CreateStore(in ? in : null, builder_.CreateStructGEP(promiseTy_, promise, kIn));
  builder_.CreateStore(ret ? ret : null, builder_.CreateStructGEP(promiseTy_, promise, kRet));
  builder_.CreateStore(builder_.getInt32(1), running);
  builder_.CreateCall(coroIntrinsic(llvm::Intrinsic::coro_resume), {frame});
  builder_.CreateStore(builder_.getInt32(0), running);
  // Its code threw: the generator is done, and the error goes on here.
  emitThrowCheck();
  llvm::Value *gave =
      builder_.CreateNot(builder_.CreateCall(coroIntrinsic(llvm::Intrinsic::coro_done), {frame}));
  llvm::BasicBlock *resumeEnd = builder_.GetInsertBlock();
  builder_.CreateBr(endBlock);

  switchTo(endBlock);
  llvm::PHINode *has = builder_.CreatePHI(builder_.getInt1Ty(), 2, "has");
  has->addIncoming(builder_.getFalse(), before);
  has->addIncoming(gave, resumeEnd);
  return has;
}

llvm::Value *CodeGen::callCallback(llvm::Value *slot, const Type *fnType,
                                   std::vector<llvm::Value *> args,
                                   llvm::ArrayRef<const Type *> types) {
  size_t passes = std::min(fnType->params().size(), args.size());
  for (size_t i = passes; i < args.size(); ++i)
    releaseValue(args[i], types[i]);
  args.resize(passes);
  llvm::Value *fn = builder_.CreateLoad(fnTy_, slot, "callback");
  llvm::Value *result = callFunctionValue(fn, fnType, args);
  emitThrowCheckIf(typeThrows(fnType));
  return result;
}

CodeGen::Producer CodeGen::arrayProducer(llvm::Value *slot, const Type *arrayType, Builtin mode) {
  Producer producer;
  producer.run = [this, slot, arrayType, mode](const Consumer &consumer) {
    const Type *element = arrayType->inner();
    llvm::Type *i64 = builder_.getInt64Ty();
    // As JavaScript's iterator, each step reads the current length, so
    // elements pushed meanwhile are visited too.
    llvm::Value *indexSlot = createStackSlot(i64, "index");
    builder_.CreateStore(builder_.getInt64(0), indexSlot);
    llvm::BasicBlock *headBlock = newBlock("iter.array");
    llvm::BasicBlock *bodyBlock = newBlock("iter.array.body");
    llvm::BasicBlock *endBlock = newBlock("iter.array.end");
    emitBlock(headBlock);
    llvm::Value *array = builder_.CreateLoad(builder_.getPtrTy(), slot, "array");
    llvm::Value *index = builder_.CreateLoad(i64, indexSlot, "index");
    builder_.CreateCondBr(builder_.CreateICmpULT(index, arrayLength(array)), bodyBlock, endBlock);
    switchTo(bodyBlock);
    builder_.CreateStore(builder_.CreateAdd(index, builder_.getInt64(1)), indexSlot);
    llvm::Value *value;
    if (mode == Builtin::ArrKeys) {
      value = builder_.CreateTrunc(index, builder_.getInt32Ty());
    } else {
      value = builder_.CreateLoad(lower(element), elementAddress(arrayData(array), element, index),
                                  "element");
      retainValue(value, element);
      if (mode == Builtin::ArrEntries) {
        // A new [index, value] each time, as in JavaScript.
        const Type *tuple = Type::tuple({Type::intTy(), element});
        llvm::Value *entry = allocObject(tuple, {0, 1});
        builder_.CreateStore(builder_.CreateTrunc(index, builder_.getInt32Ty()),
                             fieldAddress(entry, tuple, "0"));
        builder_.CreateStore(value, fieldAddress(entry, tuple, "1"));
        adoptFields(entry, tuple);
        value = entry;
      }
    }
    if (llvm::Value *sent = consumer.each(value))
      releaseValue(sent, consumer.sent);
    branchTo(headBlock);
    switchTo(endBlock);
  };
  return producer;
}

CodeGen::Producer CodeGen::valueProducer(llvm::Value *slot, const Type *type,
                                         llvm::Value *returnSlot) {
  if (type->isArray())
    return arrayProducer(slot, type, Builtin::ArrValues);
  if (type->isTypedArray())
    return bytesProducer(slot, type);
  Producer producer;
  if (type->isString()) {
    producer.run = [this, slot](const Consumer &consumer) {
      llvm::Type *i64 = builder_.getInt64Ty();
      llvm::Value *indexSlot = createStackSlot(i64, "index");
      builder_.CreateStore(builder_.getInt64(0), indexSlot);
      llvm::BasicBlock *headBlock = newBlock("iter.string");
      llvm::BasicBlock *bodyBlock = newBlock("iter.string.body");
      llvm::BasicBlock *endBlock = newBlock("iter.string.end");
      emitBlock(headBlock);
      llvm::Value *str = builder_.CreateLoad(strTy_, slot, "string");
      llvm::Value *index = builder_.CreateLoad(i64, indexSlot, "index");
      builder_.CreateCondBr(builder_.CreateICmpULT(index, stringLength(str)), bodyBlock, endBlock);
      switchTo(bodyBlock);
      // One code point: one code unit, or two for a surrogate pair.
      llvm::Value *out = createStackSlot(strTy_, "code.point");
      llvm::Value *units =
          builder_.CreateCall(rt_.strCodePointAt, {out, stringObj(str), stringMeta(str), index});
      builder_.CreateStore(builder_.CreateAdd(index, builder_.CreateSExt(units, i64)), indexSlot);
      if (llvm::Value *sent = consumer.each(builder_.CreateLoad(strTy_, out)))
        releaseValue(sent, consumer.sent);
      branchTo(headBlock);
      switchTo(endBlock);
    };
    return producer;
  }
  // An iterator object: next() until it is done; what a generator returns
  // goes to `returnSlot`, if anywhere.
  producer.run = [this, slot, type, returnSlot](const Consumer &consumer) {
    const Type *element = type->yielded();
    llvm::Value *out = createStackSlot(lower(element), "next.value");
    // What the consumer gives back goes to the next call of next(), as
    // `yield*` passes on what is sent to it.
    llvm::Value *sentSlot = nullptr, *hasSent = nullptr;
    if (consumer.sent) {
      sentSlot = createStackSlot(lower(consumer.sent), "next.sent");
      hasSent = createStackSlot(builder_.getInt1Ty(), "next.has.sent");
      builder_.CreateStore(builder_.getFalse(), hasSent);
    }
    llvm::BasicBlock *headBlock = newBlock("iter.next");
    llvm::BasicBlock *bodyBlock = newBlock("iter.next.body");
    llvm::BasicBlock *endBlock = newBlock("iter.next.end");
    emitBlock(headBlock);
    llvm::Value *it = builder_.CreateLoad(builder_.getPtrTy(), slot, "iterator");
    llvm::Value *in = nullptr;
    if (hasSent)
      in = builder_.CreateSelect(builder_.CreateLoad(builder_.getInt1Ty(), hasSent), sentSlot,
                                 llvm::ConstantPointerNull::get(builder_.getPtrTy()));
    llvm::Value *has = emitResume(it, out, in, returnSlot);
    if (hasSent) {
      // The generator took a copy of what it needed.
      llvm::BasicBlock *releaseBlock = newBlock("next.release");
      llvm::BasicBlock *afterBlock = newBlock("next.after");
      builder_.CreateCondBr(builder_.CreateLoad(builder_.getInt1Ty(), hasSent), releaseBlock,
                            afterBlock);
      switchTo(releaseBlock);
      releaseValue(builder_.CreateLoad(lower(consumer.sent), sentSlot), consumer.sent);
      builder_.CreateStore(builder_.getFalse(), hasSent);
      builder_.CreateBr(afterBlock);
      switchTo(afterBlock);
    }
    builder_.CreateCondBr(has, bodyBlock, endBlock);
    switchTo(bodyBlock);
    llvm::Value *sent = consumer.each(builder_.CreateLoad(lower(element), out, "value"));
    if (sent && blockIsOpen()) {
      builder_.CreateStore(sent, sentSlot);
      builder_.CreateStore(builder_.getTrue(), hasSent);
    }
    branchTo(headBlock);
    switchTo(endBlock);
  };
  return producer;
}

// The stages of a chain take each value from the stage before them and give
// what they make of it to the next one.
CodeGen::Consumer CodeGen::stageConsumer(Builtin stage, const Type *callbackType,
                                         const Type *element, const Type *result,
                                         llvm::Value *operand, const Consumer &down) {
  llvm::Type *i32 = builder_.getInt32Ty(), *i64 = builder_.getInt64Ty();
  Consumer stageOut;
  stageOut.stop = down.stop;
  // What the next stage gives back is not passed on: helpers call next()
  // without a value.
  auto give = [this, down](llvm::Value *value) {
    if (llvm::Value *sent = down.each(value))
      releaseValue(sent, down.sent);
  };
  // The counter callbacks get.
  llvm::Value *counter = nullptr;
  if (callbackType) {
    counter = createStackSlot(i32, "counter");
    builder_.CreateStore(builder_.getInt32(0), counter);
  }
  auto nextCount = [this, counter, i32] {
    llvm::Value *count = builder_.CreateLoad(i32, counter, "count");
    builder_.CreateStore(builder_.CreateAdd(count, builder_.getInt32(1)), counter);
    return count;
  };

  switch (stage) {
  case Builtin::IterMap:
    stageOut.each = [=, this](llvm::Value *value) -> llvm::Value * {
      give(callCallback(operand, callbackType, {value, nextCount()}, {element, Type::intTy()}));
      return nullptr;
    };
    break;
  case Builtin::IterFilter:
    stageOut.each = [=, this](llvm::Value *value) -> llvm::Value * {
      retainValue(value, element); // the callback owns what it gets
      llvm::Value *keep =
          callCallback(operand, callbackType, {value, nextCount()}, {element, Type::intTy()});
      llvm::BasicBlock *keepBlock = newBlock("filter.keep");
      llvm::BasicBlock *dropBlock = newBlock("filter.drop");
      llvm::BasicBlock *nextBlock = newBlock("filter.next");
      builder_.CreateCondBr(keep, keepBlock, dropBlock);
      switchTo(keepBlock);
      // `filter(x => x != undefined)` gives the values themselves.
      give(emitConversion(value, element, result));
      branchTo(nextBlock);
      switchTo(dropBlock);
      releaseValue(value, element);
      builder_.CreateBr(nextBlock);
      switchTo(nextBlock);
      return nullptr;
    };
    break;
  case Builtin::IterTake:
  case Builtin::IterDrop: {
    // The count left; a negative one is a RangeError, as in JavaScript.
    llvm::Value *left = createStackSlot(i64, "left");
    llvm::Value *count = builder_.CreateLoad(i64, operand, "limit");
    llvm::BasicBlock *badBlock = newBlock("limit.negative");
    llvm::BasicBlock *okBlock = newBlock("limit.ok");
    builder_.CreateCondBr(builder_.CreateICmpSLT(count, builder_.getInt64(0)), badBlock, okBlock,
                          llvm::MDBuilder(ctx_).createUnlikelyBranchWeights());
    switchTo(badBlock);
    builder_.CreateCall(processFunction("tz_iter_bad_count", builder_.getVoidTy(), {i64}), {count});
    emitRuntimeThrow();
    switchTo(okBlock);
    builder_.CreateStore(count, left);
    if (stage == Builtin::IterTake) {
      // take(0) gives nothing and asks for nothing.
      llvm::BasicBlock *zeroBlock = newBlock("take.none");
      llvm::BasicBlock *startBlock = newBlock("take.start");
      builder_.CreateCondBr(builder_.CreateICmpEQ(count, builder_.getInt64(0)), zeroBlock,
                            startBlock);
      switchTo(zeroBlock);
      down.stop();
      switchTo(startBlock);
      stageOut.each = [=, this](llvm::Value *value) -> llvm::Value * {
        llvm::Value *rest =
            builder_.CreateSub(builder_.CreateLoad(i64, left), builder_.getInt64(1));
        builder_.CreateStore(rest, left);
        give(value);
        if (!blockIsOpen())
          return nullptr;
        // After the last one, the iteration ends without asking for more.
        llvm::BasicBlock *doneBlock = newBlock("take.done");
        llvm::BasicBlock *moreBlock = newBlock("take.more");
        builder_.CreateCondBr(builder_.CreateICmpEQ(rest, builder_.getInt64(0)), doneBlock,
                              moreBlock);
        switchTo(doneBlock);
        down.stop();
        switchTo(moreBlock);
        return nullptr;
      };
    } else {
      stageOut.each = [=, this](llvm::Value *value) -> llvm::Value * {
        llvm::Value *rest = builder_.CreateLoad(i64, left);
        llvm::BasicBlock *skipBlock = newBlock("drop.skip");
        llvm::BasicBlock *passBlock = newBlock("drop.pass");
        llvm::BasicBlock *nextBlock = newBlock("drop.next");
        builder_.CreateCondBr(builder_.CreateICmpSGT(rest, builder_.getInt64(0)), skipBlock,
                              passBlock);
        switchTo(skipBlock);
        builder_.CreateStore(builder_.CreateSub(rest, builder_.getInt64(1)), left);
        releaseValue(value, element);
        builder_.CreateBr(nextBlock);
        switchTo(passBlock);
        give(value);
        branchTo(nextBlock);
        switchTo(nextBlock);
        return nullptr;
      };
    }
    break;
  }
  case Builtin::IterFlatMap:
    stageOut.each = [=, this](llvm::Value *value) -> llvm::Value * {
      // Each result is iterated to the end before the next value.
      const Type *inner = callbackType->ret();
      llvm::Value *iterable =
          callCallback(operand, callbackType, {value, nextCount()}, {element, Type::intTy()});
      pushScope();
      llvm::Value *slot = createManagedSlot(inner, "flat");
      builder_.CreateStore(iterable, slot);
      Consumer flat;
      flat.each = [=](llvm::Value *v) -> llvm::Value * {
        give(v);
        return nullptr;
      };
      flat.stop = down.stop;
      valueProducer(slot, inner).run(flat);
      popScope();
      return nullptr;
    };
    break;
  default: llvm_unreachable("not a stage of a chain");
  }
  return stageOut;
}

bool CodeGen::isInlinable(const FunctionDecl *decl) const {
  if (!decl->isGenerator || decl->isClosure() || !decl->sym || !decl->sym->ret ||
      !decl->sym->ret->isIterator())
    return false;
  // A `catch` or `finally` around a `yield` would take what the consumer's
  // code throws there, or run when it jumps out.
  if (hasTry(decl->body))
    return false;
  // Not inside itself: a recursive generator would be emitted inside itself
  // without end.
  unsigned depth = 0;
  for (const InlineGenerator *frame = inline_; frame; frame = frame->consumerContext.inlined) {
    if (frame->decl == decl)
      return false;
    ++depth;
  }
  unsigned yields = countYields(decl);
  return depth < kMaxInlineDepth && yields <= kMaxInlineYields &&
         inlineCopies_ * std::max(yields, 1u) <= kMaxInlineCopies;
}

CodeGen::Producer CodeGen::prepare(Expr *source, llvm::Value *returnSlot) {
  const Type *type = source->type;
  auto hold = [&](Expr *expr) {
    const Type *t = expr->type;
    llvm::Value *slot =
        t->isManaged() ? createManagedSlot(t, "operand") : createStackSlot(lower(t), "operand");
    builder_.CreateStore(emitExpr(expr), slot);
    return slot;
  };
  if (auto *call = dyn_cast<CallExpr>(source)) {
    switch (call->builtin) {
    case Builtin::IterFrom: // of an iterator, the iterator itself
      return prepare(call->args[0], returnSlot);
    case Builtin::StrMatchAll: {
      // The string, then the regex, then the check of its g flag, as when the
      // iterator is made.
      llvm::Value *text = hold(cast<MemberExpr>(call->callee)->object);
      llvm::Value *regex = hold(call->args[0]);
      if (!call->regex) {
        builder_.CreateCall(regexFunction("tz_re_check_match_all", builder_.getVoidTy(),
                                          {builder_.getPtrTy()}),
                            {builder_.CreateLoad(builder_.getPtrTy(), regex, "regex")});
        emitThrowCheck(); // a TypeError without the g flag
      }
      llvm::Value *lastSlot = createStackSlot(builder_.getInt32Ty(), "lastIndex");
      builder_.CreateStore(
          builder_.CreateCall(regexFunction("tz_re_match_all_begin", builder_.getInt32Ty(),
                                            {builder_.getPtrTy()}),
                              {builder_.CreateLoad(builder_.getPtrTy(), regex, "regex")}),
          lastSlot);
      return matchAllProducer(text, regex, lastSlot, matchShape(type->yielded(), call->regex));
    }
    case Builtin::ArrKeys:
    case Builtin::ArrValues:
    case Builtin::ArrEntries: {
      Expr *array = cast<MemberExpr>(call->callee)->object;
      return arrayProducer(hold(array), array->type, call->builtin);
    }
    case Builtin::MapKeys:
    case Builtin::MapValues:
    case Builtin::MapEntries:
    case Builtin::SetValues:
    case Builtin::SetEntries: {
      // Its entries stay where they are while the loop runs.
      Expr *dict = cast<MemberExpr>(call->callee)->object;
      llvm::Value *slot = hold(dict);
      beginDictIteration(slot);
      return dictProducer(slot, dict->type, call->builtin);
    }
    case Builtin::IterMap:
    case Builtin::IterFilter:
    case Builtin::IterTake:
    case Builtin::IterDrop:
    case Builtin::IterFlatMap: {
      // The chain before it first, then its own argument, as JavaScript
      // evaluates them.
      Expr *receiver = cast<MemberExpr>(call->callee)->object;
      Producer from = prepare(receiver);
      llvm::Value *operand = hold(call->args[0]);
      Builtin stage = call->builtin;
      const Type *callback = call->args[0]->type->isFunction() ? call->args[0]->type : nullptr;
      const Type *element = receiver->type->yielded(), *result = call->type->yielded();
      Producer producer;
      producer.run = [this, from, stage, callback, element, result, operand](const Consumer &down) {
        from.run(stageConsumer(stage, callback, element, result, operand, down));
      };
      return producer;
    }
    case Builtin::None:
      if (call->func && isInlinable(call->func->decl)) {
        // The generator's code goes where it is consumed. Its arguments are
        // evaluated and its parameters bound now, as when it is called.
        FunctionDecl *decl = call->func->decl;
        auto snapshot = std::make_shared<std::tuple<decltype(addresses_), decltype(boxSlots_),
                                                    decltype(envBoxes_), decltype(groupEnvs_)>>(
            addresses_, boxSlots_, envBoxes_, groupEnvs_);
        std::vector<llvm::Value *> args;
        for (Expr *arg : call->args)
          args.push_back(emitExpr(arg));
        for (size_t i = args.size(); i < call->func->params.size(); ++i)
          args.push_back(undefinedValue(call->func->params[i]));
        emitParams(decl, args);
        Producer producer;
        producer.run = [this, decl, returnSlot, snapshot](const Consumer &consumer) {
          emitInlineGenerator(decl, consumer, returnSlot);
          // The same generator may be emitted inline again inside the
          // consumer: its variables are where they were for the rest.
          std::tie(addresses_, boxSlots_, envBoxes_, groupEnvs_) = *snapshot;
        };
        return producer;
      }
      break;
    default: break;
    }
  }
  assert((type->isArray() || type->isString() || type->isIterator() || type->isTypedArray()) &&
         "Sema checks what is iterated");
  llvm::Value *slot = hold(source);
  // An iterator stopped before its end is closed (iterCloseFunction) when
  // the iteration's scope ends, however it ends, even if it lives on in a
  // variable.
  if (type->isIterator())
    scopes_.back().iterators.push_back(slot);
  return valueProducer(slot, type, returnSlot);
}

// The body of a generator in place, in a context of its own; at each `yield`
// the consumer runs in the context the iteration started in.
void CodeGen::emitInlineGenerator(FunctionDecl *decl, const Consumer &consumer,
                                  llvm::Value *returnSlot) {
  InlineGenerator frame{decl,       consumer,      saveContext(), newBlock("gen.done"),
                        returnSlot, scopes_.size()};
  currentFn_ = decl;
  envParam_ = nullptr;
  returnType_ = decl->sym->ret;
  tailGroup_ = nullptr;
  targets_.clear();
  inline_ = &frame;
  unsigned copies = inlineCopies_;
  inlineCopies_ *= std::max(countYields(decl), 1u);
  emitStmt(decl->body);
  inlineCopies_ = copies;
  branchTo(frame.exit);
  switchTo(frame.exit);
  restoreContext(frame.consumerContext);
}

llvm::Value *CodeGen::emitYieldValue(llvm::Value *value, const Type *resultType) {
  if (!inline_)
    return emitCoroutineYield(value, resultType);
  InlineGenerator *frame = inline_;
  EmitContext generator = saveContext();
  restoreContext(frame->consumerContext);
  llvm::Value *sent = frame->consumer.each(value);
  const Type *sentType = frame->consumer.sent;
  restoreContext(generator);
  // What the consumer gave back, as the `yield` wants it.
  if (!resultType || resultType->isUndefined()) {
    if (sent)
      releaseValue(sent, sentType);
    return resultType ? builder_.getFalse() : nullptr;
  }
  if (!sent)
    return undefinedValue(resultType);
  return emitConversion(sent, sentType, resultType);
}

llvm::Value *CodeGen::emitYield(YieldExpr *yield) {
  if (async_ && async_->generator)
    return emitAsyncYield(yield);
  const Type *generator = currentFn_->sym->ret;
  const Type *yielded = generator->yielded();
  if (!yield->delegate) {
    llvm::Value *value = yield->value ? emitExpr(yield->value) : undefinedValue(yielded);
    return emitYieldValue(value, yield->type);
  }
  // `yield* xs`: each value of xs, and then what xs returns.
  const Type *iterable = yield->value->type;
  const Type *innerSent =
      iterable->isIterator() && !iterable->sent()->isVoid() ? iterable->sent() : nullptr;
  pushScope();
  llvm::Value *returnSlot = nullptr;
  if (!yield->type->isUndefined())
    returnSlot = createManagedSlot(yield->type, "yield.result");
  Producer producer = prepare(yield->value, returnSlot);
  Consumer consumer;
  consumer.sent = innerSent;
  const Type *element = yield->element;
  consumer.each = [this, element, yielded, innerSent](llvm::Value *value) {
    return emitYieldValue(emitConversion(value, element, yielded), innerSent);
  };
  // `yield* xs.take(2)` ends when take does.
  size_t depth = scopes_.size();
  llvm::BasicBlock *endBlock = newBlock("yield.all.end");
  consumer.stop = [this, depth, endBlock] {
    emitCleanups(depth);
    builder_.CreateBr(endBlock);
  };
  producer.run(consumer);
  emitBlock(endBlock);
  llvm::Value *result = builder_.getFalse(); // undefined
  if (returnSlot) {
    // Moved out of the slot, which the scope would release.
    result = builder_.CreateLoad(lower(yield->type), returnSlot);
    builder_.CreateStore(llvm::Constant::getNullValue(lower(yield->type)), returnSlot);
  }
  popScope();
  return result;
}

void CodeGen::emitIteratorForOf(ForOfStmt *stmt) {
  llvm::SmallVector<llvm::StringRef, 1> labels = std::move(pendingLabels_);
  pendingLabels_.clear();
  pushScope(); // the operands of the iteration
  Producer producer = prepare(stmt->iterable);
  size_t depth = scopes_.size();
  llvm::BasicBlock *endBlock = newBlock("for.of.end");
  Consumer consumer;
  consumer.stop = [this, depth, endBlock] {
    emitCleanups(depth);
    builder_.CreateBr(endBlock);
  };
  consumer.each = [this, stmt, labels, depth, endBlock](llvm::Value *value) -> llvm::Value * {
    // `continue` goes on after the value, in a generator emitted inline
    // inside its scopes; `break` leaves them all.
    llvm::BasicBlock *nextBlock = newBlock("for.of.next");
    size_t here = scopes_.size();
    pushScope(); // the loop variable, fresh in every iteration
    storeVar(stmt->binding.sym, createLocal(stmt->binding.sym, stmt->binding.name), value,
             /*replacing=*/true);
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

void CodeGen::emitIteratorSpread(llvm::Value *array, Expr *source, const Type *element) {
  pushScope();
  Producer producer = prepare(source);
  size_t depth = scopes_.size();
  llvm::BasicBlock *endBlock = newBlock("spread.end");
  const Type *yielded = iteratedType(source->type);
  Consumer consumer;
  consumer.each = [this, array, yielded, element](llvm::Value *value) -> llvm::Value * {
    emitPush(array, {emitConversion(value, yielded, element)}, element);
    return nullptr;
  };
  consumer.stop = [this, depth, endBlock] {
    emitCleanups(depth);
    builder_.CreateBr(endBlock);
  };
  producer.run(consumer);
  emitBlock(endBlock);
  popScope();
}

llvm::Value *CodeGen::emitIteratorBuiltin(CallExpr *call) {
  switch (call->builtin) {
  case Builtin::IterNext: return emitNext(call);
  case Builtin::ArrayFrom:
  case Builtin::IterToArray:
  case Builtin::IterForEach:
  case Builtin::IterReduce:
  case Builtin::IterSome:
  case Builtin::IterEvery:
  case Builtin::IterFind: return emitIteratorSink(call);
  default: return emitIteratorObject(call);
  }
}

// toArray, forEach, reduce, some, every, find and Array.from: the chain
// before them runs here as one loop.
llvm::Value *CodeGen::emitIteratorSink(CallExpr *call) {
  Builtin method = call->builtin;
  bool from = method == Builtin::ArrayFrom;
  Expr *source = from ? call->args[0] : cast<MemberExpr>(call->callee)->object;
  const Type *element = iteratedType(source->type);
  pushScope(); // the operands
  Producer producer = prepare(source);
  // The callback is evaluated after the chain, as JavaScript does.
  Expr *callbackExpr = from ? (call->args.size() > 1 ? call->args[1] : nullptr)
                       : method == Builtin::IterToArray ? nullptr
                                                        : call->args[0];
  const Type *callbackType = callbackExpr ? callbackExpr->type : nullptr;
  llvm::Value *callback = nullptr;
  if (callbackExpr) {
    callback = createManagedSlot(callbackType, "callback");
    builder_.CreateStore(emitExpr(callbackExpr), callback);
  }
  llvm::Type *i32 = builder_.getInt32Ty();
  llvm::Value *counter = createStackSlot(i32, "counter");
  builder_.CreateStore(builder_.getInt32(0), counter);
  auto nextCount = [this, counter, i32] {
    llvm::Value *count = builder_.CreateLoad(i32, counter, "count");
    builder_.CreateStore(builder_.CreateAdd(count, builder_.getInt32(1)), counter);
    return count;
  };
  size_t depth = scopes_.size();
  llvm::BasicBlock *endBlock = newBlock("iter.end");
  Consumer consumer;
  consumer.stop = [this, depth, endBlock] {
    emitCleanups(depth);
    builder_.CreateBr(endBlock);
  };

  llvm::Value *array = nullptr, *resultSlot = nullptr, *hasAcc = nullptr;
  const Type *resultType = call->type;
  // The array being filled waits in a slot, which an error releases.
  Held heldArray{nullptr, nullptr, nullptr};
  switch (method) {
  case Builtin::ArrayFrom:
  case Builtin::IterToArray: {
    const Type *stored = resultType->inner();
    array = newArray(Type::array(stored), builder_.getInt64(0), builder_.getInt64(0));
    heldArray = hold(array, resultType, true);
    consumer.each = [=, this](llvm::Value *value) -> llvm::Value * {
      if (callback)
        value =
            callCallback(callback, callbackType, {value, nextCount()}, {element, Type::intTy()});
      else
        value = emitConversion(value, element, stored);
      emitPush(array, {value}, stored);
      return nullptr;
    };
    break;
  }
  case Builtin::IterForEach:
    consumer.each = [=, this](llvm::Value *value) -> llvm::Value * {
      if (llvm::Value *result =
              callCallback(callback, callbackType, {value, nextCount()}, {element, Type::intTy()}))
        releaseValue(result, callbackType->ret());
      return nullptr;
    };
    break;
  case Builtin::IterReduce: {
    resultSlot = createStackSlot(lower(resultType), "acc");
    if (call->args.size() > 1) {
      builder_.CreateStore(emitExpr(call->args[1]), resultSlot);
    } else {
      // Without an initial value the first value is the accumulator.
      hasAcc = createStackSlot(builder_.getInt1Ty(), "has.acc");
      builder_.CreateStore(builder_.getFalse(), hasAcc);
    }
    consumer.each = [=, this](llvm::Value *value) -> llvm::Value * {
      llvm::BasicBlock *firstBlock = nullptr, *nextBlock = nullptr;
      if (hasAcc) {
        firstBlock = newBlock("reduce.first");
        nextBlock = newBlock("reduce.next");
        llvm::BasicBlock *combineBlock = newBlock("reduce.combine");
        builder_.CreateCondBr(builder_.CreateLoad(builder_.getInt1Ty(), hasAcc), combineBlock,
                              firstBlock);
        switchTo(firstBlock);
        builder_.CreateStore(value, resultSlot);
        builder_.CreateStore(builder_.getTrue(), hasAcc);
        nextCount();
        builder_.CreateBr(nextBlock);
        switchTo(combineBlock);
      }
      llvm::Value *acc = builder_.CreateLoad(lower(resultType), resultSlot); // moves into the call
      builder_.CreateStore(callCallback(callback, callbackType, {acc, value, nextCount()},
                                        {resultType, element, Type::intTy()}),
                           resultSlot);
      if (nextBlock)
        emitBlock(nextBlock);
      return nullptr;
    };
    break;
  }
  case Builtin::IterSome:
  case Builtin::IterEvery:
  case Builtin::IterFind: {
    resultSlot = createStackSlot(lower(resultType), "result");
    builder_.CreateStore(method == Builtin::IterSome    ? builder_.getFalse()
                         : method == Builtin::IterEvery ? builder_.getTrue()
                                                        : undefinedValue(resultType),
                         resultSlot);
    consumer.each = [=, this](llvm::Value *value) -> llvm::Value * {
      bool find = method == Builtin::IterFind;
      if (find)
        retainValue(value, element); // kept if it is the one
      llvm::Value *decides =
          callCallback(callback, callbackType, {value, nextCount()}, {element, Type::intTy()});
      if (method == Builtin::IterEvery)
        decides = builder_.CreateNot(decides);
      llvm::BasicBlock *foundBlock = newBlock("iter.found");
      llvm::BasicBlock *otherBlock = newBlock("iter.other");
      builder_.CreateCondBr(decides, foundBlock, otherBlock);
      switchTo(foundBlock);
      builder_.CreateStore(find ? makeOptional(value, builder_.getTrue(), resultType)
                                : builder_.getInt1(method == Builtin::IterSome),
                           resultSlot);
      consumer.stop();
      switchTo(otherBlock);
      if (find)
        releaseValue(value, element);
      return nullptr;
    };
    break;
  }
  default: llvm_unreachable("not a consumer of an iterator");
  }
  producer.run(consumer);
  emitBlock(endBlock);
  llvm::Value *result = nullptr;
  if (array) {
    result = take(heldArray);
  } else if (resultSlot) {
    if (hasAcc) {
      // As JavaScript, which reports a TypeError.
      llvm::BasicBlock *emptyBlock = newBlock("reduce.empty");
      llvm::BasicBlock *okBlock = newBlock("reduce.ok");
      builder_.CreateCondBr(builder_.CreateLoad(builder_.getInt1Ty(), hasAcc), okBlock, emptyBlock);
      switchTo(emptyBlock);
      emitThrowNew(1, "Reduce of a done iterator with no initial value");
      switchTo(okBlock);
    }
    result = builder_.CreateLoad(lower(resultType), resultSlot);
  }
  popScope();
  return result;
}

// `it.next(x)`: a new { value, done }.
llvm::Value *CodeGen::emitNext(CallExpr *call) {
  auto *member = cast<MemberExpr>(call->callee);
  const Type *type = member->object->type;
  const Type *element = type->yielded(), *returned = type->returned(), *sent = type->sent();
  // Owned: the generator may drop every other reference to itself while it
  // runs. It waits in a slot, which an error from it releases.
  llvm::Value *it = emitExpr(member->object);
  Held heldIt = hold(it, type, true);
  llvm::Value *sentValue = nullptr, *in = nullptr;
  if (!call->args.empty()) {
    sentValue = emitExpr(call->args[0]);
    if (sent->isVoid()) {
      releaseValue(sentValue, call->args[0]->type); // the generator ignores it
      sentValue = nullptr;
    } else {
      in = spill(sentValue, "sent");
    }
  }
  // Zero unless it gives a value: an undefined value holds zero.
  llvm::Value *out = createStackSlot(lower(element), "next.value");
  builder_.CreateStore(llvm::Constant::getNullValue(lower(element)), out);
  llvm::Value *ret = nullptr;
  if (!returned->isVoid()) {
    ret = createStackSlot(lower(returned), "next.return");
    builder_.CreateStore(llvm::Constant::getNullValue(lower(returned)), ret);
  }
  llvm::Value *frame =
      builder_.CreateConstInBoundsGEP1_64(builder_.getInt8Ty(), it, kHeaderSize, "frame");
  llvm::Value *wasDone =
      builder_.CreateCall(coroIntrinsic(llvm::Intrinsic::coro_done), {frame}, "was.done");
  llvm::Value *has = emitResume(it, out, in, ret);
  if (sentValue)
    releaseValue(sentValue, sent); // the generator took a copy of it
  // `T | R | undefined`: what it gave, or what it returned.
  const Type *valueType = call->type->field("value")->type;
  llvm::Value *given =
      emitConversion(builder_.CreateLoad(lower(element), out), element, valueType);
  llvm::Value *value = builder_.CreateSelect(has, given, undefinedValue(valueType));
  if (ret) {
    // What a generator returns comes with the call that finished it.
    llvm::Value *returnedValue =
        emitConversion(builder_.CreateLoad(lower(returned), ret), returned, valueType);
    returnedValue = builder_.CreateSelect(wasDone, undefinedValue(valueType), returnedValue);
    value = builder_.CreateSelect(has, value, returnedValue);
  }
  const Type *resultType = call->type;
  llvm::Value *result = allocObject(resultType, {0, 1});
  builder_.CreateStore(value, fieldAddress(result, resultType, "value"));
  builder_.CreateStore(builder_.CreateNot(has), fieldAddress(result, resultType, "done"));
  adoptFields(result, resultType);
  releaseValue(take(heldIt), type);
  return result;
}

// The iterator of an array or a string, or a helper, as a value.
llvm::Value *CodeGen::emitIteratorObject(CallExpr *call) {
  const Type *result = call->type;
  switch (call->builtin) {
  case Builtin::IterFrom: {
    Expr *arg = call->args[0];
    if (arg->type->isIterator())
      return emitExpr(arg); // its own iterator
    Builtin kind = arg->type->isString() ? Builtin::IterFrom : Builtin::ArrValues;
    return builder_.CreateCall(iteratorFunction(kind, arg->type, nullptr, result), {emitExpr(arg)});
  }
  case Builtin::ArrKeys:
  case Builtin::ArrValues:
  case Builtin::ArrEntries:
  case Builtin::MapKeys:
  case Builtin::MapValues:
  case Builtin::MapEntries:
  case Builtin::SetValues:
  case Builtin::SetEntries: {
    Expr *array = cast<MemberExpr>(call->callee)->object;
    return builder_.CreateCall(iteratorFunction(call->builtin, array->type, nullptr, result),
                               {emitExpr(array)});
  }
  default: {
    // A stage: the iterator before it, and its callback or count.
    Expr *receiver = cast<MemberExpr>(call->callee)->object;
    Expr *arg = call->args[0];
    llvm::Function *fn = iteratorFunction(call->builtin, receiver->type, arg->type, result);
    llvm::Value *source = emitExpr(receiver);
    return builder_.CreateCall(fn, {source, emitExpr(arg)});
  }
  }
}

} // namespace tz
