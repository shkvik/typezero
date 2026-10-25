#include "compiler/lower/iterators/generator_lowerer/generator_lowerer.h"
#include "compiler/check/checker/checker.h"
#include "compiler/library/regex/regex_lowerer/regex_lowerer.h"
#include "compiler/lower/errors/throw_lowerer/throw_lowerer.h"
#include "compiler/lower/errors/unwind_lowerer/unwind_lowerer.h"
#include "compiler/lower/function_builder/function_builder.h"
#include "compiler/lower/function_frame/function_frame.h"
#include "compiler/lower/functions/env_lowerer/env_lowerer.h"
#include "compiler/lower/iterators/async_generator_lowerer/async_generator_lowerer.h"
#include "compiler/lower/iterators/coroutine_lowerer/coroutine_lowerer.h"
#include "compiler/lower/iterators/iter_lower_helpers/iter_lower_helpers.h"
#include "compiler/lower/iterators/lazy_chain_lowerer/lazy_chain_lowerer.h"
#include "compiler/lower/module_builder/module_builder.h"
#include "compiler/lower/objects/dict_lowerer/dict_lowerer.h"
#include "compiler/lower/stmt/stmt_lowerer/stmt_lowerer.h"
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

// What an iterator prints as; must match TzLayout in runtime/core/tzrt/tzrt.h.
enum : unsigned {
  kLayoutGenerator = 2,       // Object [Generator] {}
  kLayoutArrayIterator = 4,   // Object [Array Iterator] {}
  kLayoutStringIterator = 8,  // Object [String Iterator] {}
  kLayoutHelper = 16,         // Object [Iterator Helper] {}
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

} // namespace

GeneratorLowerer::GeneratorLowerer(Lowering &lowering) : lowering_(lowering) {}

llvm::Constant *GeneratorLowerer::iteratorLayout(unsigned flags) {
  llvm::Constant *&layout = iteratorLayouts_[flags];
  if (layout) {
    return layout;
  }
  if (!iterDropFn_) {
    auto *type = llvm::FunctionType::get(lowering_.builder_.getVoidTy(),
                                         {lowering_.builder_.getPtrTy()}, false);
    iterDropFn_ = llvm::Function::Create(type, llvm::GlobalValue::InternalLinkage,
                                         "tz.iter.destroy", lowering_.module_.get());
    iterDropFn_->addFnAttr(llvm::Attribute::NoUnwind);
    llvm::IRBuilder<> b(llvm::BasicBlock::Create(lowering_.ctx_, "entry", iterDropFn_));
    // Closed first, if it waits at a `yield` with a `finally` around it.
    b.CreateCall(iterCloseFunction(), {iterDropFn_->getArg(0)});
    llvm::Value *frame =
        b.CreateConstInBoundsGEP1_64(b.getInt8Ty(), iterDropFn_->getArg(0), kHeaderSize, "frame");
    b.CreateCall(lowering_.coroutineLowerer().coroIntrinsic(llvm::Intrinsic::coro_destroy),
                 {frame});
    b.CreateRetVoid();
  }
  auto *global = new llvm::GlobalVariable(
      *lowering_.module_, lowering_.layoutDescTy_, /*isConstant=*/true,
      llvm::GlobalValue::PrivateLinkage,
      llvm::ConstantStruct::get(lowering_.layoutDescTy_,
                                {iterDropFn_,
                                 llvm::ConstantPointerNull::get(lowering_.builder_.getPtrTy()),
                                 lowering_.builder_.getInt32(0), lowering_.builder_.getInt32(flags),
                                 // not an instance of a class
                                 llvm::ConstantPointerNull::get(lowering_.builder_.getPtrTy()),
                                 lowering_.builder_.getInt32(0), lowering_.builder_.getInt32(0)}),
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
llvm::Function *GeneratorLowerer::iterCloseFunction() {
  if (iterCloseFn_) {
    return iterCloseFn_;
  }
  auto *type = llvm::FunctionType::get(lowering_.builder_.getVoidTy(),
                                       {lowering_.builder_.getPtrTy()}, false);
  iterCloseFn_ = llvm::Function::Create(type, llvm::GlobalValue::InternalLinkage, "tz.iter.close",
                                        lowering_.module_.get());
  iterCloseFn_->addFnAttr(llvm::Attribute::NoUnwind);
  llvm::BasicBlock *entry = llvm::BasicBlock::Create(lowering_.ctx_, "entry", iterCloseFn_);
  llvm::BasicBlock *checkBlock = llvm::BasicBlock::Create(lowering_.ctx_, "waits", iterCloseFn_);
  llvm::BasicBlock *closeBlock = llvm::BasicBlock::Create(lowering_.ctx_, "close", iterCloseFn_);
  llvm::BasicBlock *failedBlock =
      llvm::BasicBlock::Create(lowering_.ctx_, "close.failed", iterCloseFn_);
  llvm::BasicBlock *closedBlock = llvm::BasicBlock::Create(lowering_.ctx_, "closed", iterCloseFn_);
  llvm::BasicBlock *endBlock = llvm::BasicBlock::Create(lowering_.ctx_, "end", iterCloseFn_);
  llvm::IRBuilder<> b(entry);
  llvm::Type *ptr = b.getPtrTy(), *i32 = b.getInt32Ty();
  llvm::Value *frame =
      b.CreateConstInBoundsGEP1_64(b.getInt8Ty(), iterCloseFn_->getArg(0), kHeaderSize, "frame");
  b.CreateCondBr(
      b.CreateCall(lowering_.coroutineLowerer().coroIntrinsic(llvm::Intrinsic::coro_done), {frame},
                   "done"),
      endBlock, checkBlock);
  b.SetInsertPoint(checkBlock);
  llvm::Value *promise =
      b.CreateCall(lowering_.coroutineLowerer().coroIntrinsic(llvm::Intrinsic::coro_promise),
                   {frame, b.getInt32(kFrameAlign), b.getFalse()}, "promise");
  llvm::Value *close = b.CreateStructGEP(lowering_.promiseTy_, promise, kClose);
  b.CreateCondBr(b.CreateICmpEQ(b.CreateLoad(i32, close), b.getInt32(1)), closeBlock, endBlock);
  b.SetInsertPoint(closeBlock);
  llvm::Value *saved = b.CreateLoad(ptr, lowering_.throwLowerer().thrownSlot(b), "saved");
  llvm::Value *null = llvm::ConstantPointerNull::get(b.getPtrTy());
  b.CreateStore(null, lowering_.throwLowerer().thrownSlot(b));
  b.CreateStore(b.getInt32(2), close);
  for (unsigned field : {kOut, kIn, kRet}) {
    b.CreateStore(null, b.CreateStructGEP(lowering_.promiseTy_, promise, field));
  }
  llvm::Value *running = b.CreateStructGEP(lowering_.promiseTy_, promise, kRunning);
  b.CreateStore(b.getInt32(1), running);
  b.CreateCall(lowering_.coroutineLowerer().coroIntrinsic(llvm::Intrinsic::coro_resume), {frame});
  b.CreateStore(b.getInt32(0), running);
  // A `yield` in a `finally` stopped it: it stays where it is, as in
  // JavaScript, until it is dropped.
  b.CreateStore(b.getInt32(0), close);
  llvm::Value *error = b.CreateLoad(ptr, lowering_.throwLowerer().thrownSlot(b), "error");
  b.CreateCondBr(b.CreateIsNotNull(error), failedBlock, closedBlock);
  b.SetInsertPoint(failedBlock);
  b.CreateCall(lowering_.objReleaseFn_, {error});
  b.CreateBr(closedBlock);
  b.SetInsertPoint(closedBlock);
  b.CreateStore(saved, lowering_.throwLowerer().thrownSlot(b));
  b.CreateBr(endBlock);
  b.SetInsertPoint(endBlock);
  b.CreateRetVoid();

  return iterCloseFn_;
}

// A generator function as a coroutine: a call binds the parameters and
// returns the generator, which runs the body up to the first `yield` when
// next() is first called.
void GeneratorLowerer::emitGenerator(FunctionDecl *fn, llvm::Function *function) {
  if (fn->isAsync) {
    lowering_.asyncGeneratorLowerer().emitAsyncGenerator(fn, function);
    return;
  }
  lowering_.functionBuilder().beginFunction(function, fn);
  Lowering::Coroutine coroutine{};
  lowering_.coro_ = &coroutine;
  const Type *type = fn->sym->ret;
  lowering_.coroutineLowerer().beginCoroutine(
      kLayoutGenerator, type->isIterator() && !type->sent()->isVoid() ? type->sent() : nullptr);
  coroutine.closes = hasTry(fn->body);
  coroutine.yielded = type->isIterator() ? type->yielded() : nullptr;
  lowering_.functionFrame().pushScope(); // the parameters, and the environment of a closure
  std::vector<llvm::Value *> args;
  unsigned first = 0;
  if (fn->isClosure() || fn->takesThis()) {
    lowering_.envParam_ = lowering_.function_->getArg(0);
    if (fn->isClosure()) {
      lowering_.envLowerer().bindCaptures(fn);
    } else {
      lowering_.envParam_->setName("this");
    }
    // The function value that made the generator may go before it does, and
    // so may the object of a method, which a call only lends.
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
  lowering_.unwindLowerer().finishScopeUnwind(0);
  lowering_.scopes_.pop_back(); // every way out released it
  lowering_.unwindLowerer().finishFunctionUnwind();
  lowering_.coroutineLowerer().finishCoroutine();
  lowering_.coro_ = nullptr;
  lowering_.function_ = nullptr;
}

llvm::Function *GeneratorLowerer::iteratorFunction(Builtin builtin, const Type *source,
                                                   const Type *operand, const Type *result) {
  llvm::Function *&fn = iteratorFunctions_[{builtin, source, operand, result}];
  if (fn) {
    return fn;
  }
  std::vector<llvm::Type *> params{lowering_.moduleBuilder().lower(source)};
  if (operand) {
    params.push_back(lowering_.moduleBuilder().lower(operand));
  }
  if (builtin == Builtin::StrMatchAll) {
    params.push_back(lowering_.builder_.getPtrTy()); // the shape of the matches
  }
  const char *name = "tz.iter";
  switch (builtin) {
  case Builtin::ArrKeys:
    name = "tz.iter.keys";
    break;
  case Builtin::ArrValues:
    name = "tz.iter.values";
    break;
  case Builtin::ArrEntries:
    name = "tz.iter.entries";
    break;
  case Builtin::IterFrom:
    name = "tz.iter.string";
    break;
  case Builtin::IterMap:
    name = "tz.iter.map";
    break;
  case Builtin::IterFilter:
    name = "tz.iter.filter";
    break;
  case Builtin::IterTake:
    name = "tz.iter.take";
    break;
  case Builtin::IterDrop:
    name = "tz.iter.drop";
    break;
  case Builtin::IterFlatMap:
    name = "tz.iter.flatMap";
    break;
  case Builtin::StrMatchAll:
    name = "tz.iter.matchAll";
    break;
  case Builtin::MapKeys:
    name = "tz.iter.mapKeys";
    break;
  case Builtin::MapValues:
    name = "tz.iter.mapValues";
    break;
  case Builtin::MapEntries:
    name = "tz.iter.mapEntries";
    break;
  case Builtin::SetValues:
    name = "tz.iter.setValues";
    break;
  case Builtin::SetEntries:
    name = "tz.iter.setEntries";
    break;
  default:
    break;
  }
  fn = llvm::Function::Create(llvm::FunctionType::get(lowering_.builder_.getPtrTy(), params, false),
                              llvm::GlobalValue::InternalLinkage, name, lowering_.module_.get());
  lowering_.functionBuilder().setFunctionAttributes(fn);
  lowering_.pendingIterators_.push_back({fn, builtin, source, operand, result});

  return fn;
}

// The iterator of an array or a string, or of a helper over another
// iterator, as an object: a coroutine that holds its operands.
void GeneratorLowerer::emitPendingIterators() {
  while (!lowering_.pendingIterators_.empty()) {
    Lowering::IteratorJob job = lowering_.pendingIterators_.back();
    lowering_.pendingIterators_.pop_back();
    lowering_.functionBuilder().beginFunction(job.function, nullptr);
    Lowering::Coroutine coroutine{};
    lowering_.coro_ = &coroutine;
    bool stage = isIteratorStage(job.builtin);
    bool matches = job.builtin == Builtin::StrMatchAll;
    bool dict = isDictIterator(job.builtin);
    // The iterator of a dictionary keeps the map and its position in the
    // promise too, where printing finds them: { ..., ptr map, i32 position }.
    llvm::StructType *dictPromise = nullptr;
    unsigned flags = 0;
    if (dict) {
      llvm::Type *ptr = lowering_.builder_.getPtrTy(), *i32 = lowering_.builder_.getInt32Ty();
      dictPromise = llvm::StructType::get(lowering_.ctx_, {ptr, ptr, ptr, i32, i32, ptr, i32, i32});
      flags = kLayoutDictIterator | (job.builtin == Builtin::MapValues ? kLayoutDictValues : 0) |
              (job.builtin == Builtin::MapEntries || job.builtin == Builtin::SetEntries
                   ? kLayoutDictEntries
                   : 0) |
              (job.source->isSet() ? kLayoutDictOfSet : 0);
    }
    lowering_.coroutineLowerer().beginCoroutine(stage                    ? kLayoutHelper
                                                : matches                ? kLayoutRegexIterator
                                                : dict                   ? flags
                                                : job.source->isString() ? kLayoutStringIterator
                                                                         : kLayoutArrayIterator,
                                                nullptr, dictPromise);
    lowering_.functionFrame().pushScope();
    auto hold = [&](llvm::Value *value, const Type *type, const char *name) {
      llvm::Value *slot = type->isManaged()
                              ? lowering_.functionFrame().createManagedSlot(type, name)
                              : lowering_.functionFrame().createStackSlot(
                                    lowering_.moduleBuilder().lower(type), name);
      lowering_.builder_.CreateStore(value, slot);

      return slot;
    };
    llvm::Value *source = hold(lowering_.function_->getArg(0), job.source, "source");
    llvm::Value *operand =
        job.operand ? hold(lowering_.function_->getArg(1), job.operand, "operand") : nullptr;
    // `s.matchAll(re)`: the string and the regex, whose g flag was checked;
    // the lastIndex of the copy is taken when the iterator is made.
    llvm::Value *lastSlot = nullptr;
    if (matches) {
      lastSlot =
          lowering_.functionFrame().createStackSlot(lowering_.builder_.getInt32Ty(), "lastIndex");
      llvm::Value *regex =
          lowering_.builder_.CreateLoad(lowering_.builder_.getPtrTy(), operand, "regex");
      lowering_.builder_.CreateStore(
          lowering_.builder_.CreateCall(
              lowering_.regexLowerer().regexFunction("tz_re_match_all_begin",
                                                     lowering_.builder_.getInt32Ty(),
                                                     {lowering_.builder_.getPtrTy()}),
              {regex}),
          lastSlot);
    }
    // The map of a dictionary's iterator is there to be printed before the
    // iterator first runs.
    llvm::Value *mapField = nullptr;
    if (dict) {
      mapField = lowering_.builder_.CreateStructGEP(dictPromise, coroutine.promise, 5);
      lowering_.builder_.CreateStore(
          lowering_.builder_.CreateLoad(lowering_.builder_.getPtrTy(), source), mapField);
    }
    lowering_.coroutineLowerer().emitSuspend();
    Lowering::Consumer yield;
    yield.each = [&](llvm::Value *value) {
      return lowering_.coroutineLowerer().emitCoroutineYield(value, nullptr);
    };
    yield.stop = [&] {
      lowering_.coroutineLowerer().emitCoroutineReturn(nullptr, nullptr);
      lowering_.builder_.CreateUnreachable();
    };
    if (matches) {
      lowering_.regexLowerer()
          .matchAllProducer(source, operand, lastSlot, lowering_.function_->getArg(2))
          .run(yield);
    } else if (dict) {
      // The entries stay where they are while the iterator lives, so that
      // its position stays good, and the map is read from the promise.
      lowering_.dictLowerer().beginDictIteration(source);
      llvm::Value *position = lowering_.builder_.CreateStructGEP(dictPromise, coroutine.promise, 6);
      lowering_.dictLowerer().dictProducer(mapField, job.source, job.builtin, position).run(yield);
      if (lowering_.functionFrame().blockIsOpen()) { // done: nothing is left to show
        lowering_.builder_.CreateStore(
            llvm::ConstantPointerNull::get(lowering_.builder_.getPtrTy()), mapField);
      }
    } else if (!stage) {
      Lowering::Producer producer =
          job.source->isArray()
              ? lowering_.lazyChainLowerer().arrayProducer(source, job.source, job.builtin)
              : lowering_.lazyChainLowerer().valueProducer(source, job.source);
      producer.run(yield);
    } else {
      // The stage before it is the iterator it holds.
      const Type *callback = job.operand->isFunction() ? job.operand : nullptr;
      lowering_.lazyChainLowerer()
          .valueProducer(source, job.source)
          .run(lowering_.lazyChainLowerer().stageConsumer(
              job.builtin, callback, job.source->yielded(), job.result->yielded(), operand, yield));
    }
    if (lowering_.functionFrame().blockIsOpen()) {
      lowering_.coroutineLowerer().emitCoroutineReturn(nullptr, nullptr);
    }
    lowering_.unwindLowerer().finishScopeUnwind(0);
    lowering_.scopes_.pop_back();
    lowering_.unwindLowerer().finishFunctionUnwind();
    lowering_.coroutineLowerer().finishCoroutine();
    lowering_.coro_ = nullptr;
    lowering_.function_ = nullptr;
  }
}

} // namespace tz
