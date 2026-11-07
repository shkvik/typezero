#include "compiler/lower/iterators/generator_lowerer/generator_lowerer.h"
#include "compiler/check/checker/checker.h"
#include "compiler/library/regex/regex_lowerer/regex_lowerer.h"
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


namespace tz {

namespace {

enum : unsigned {
  kLayoutGenerator = 2,
  kLayoutArrayIterator = 4,
  kLayoutStringIterator = 8,
  kLayoutHelper = 16,
  kLayoutRegexIterator = 256,
  kLayoutDictIterator = 8192,
  kLayoutDictValues = 16384,
  kLayoutDictEntries = 32768,
  kLayoutDictOfSet = 65536,
};

constexpr uint64_t kHeaderSize = 16;

constexpr uint32_t kFrameAlign = 8;

enum PromiseField : unsigned { kOut, kIn, kRet, kRunning };

}

GeneratorLowerer::GeneratorLowerer(Lowering &owner)
    : lowering(owner) {}

llvm::Constant *GeneratorLowerer::iteratorLayout(unsigned flags) {
  llvm::Constant *&layout = this->iteratorLayouts[flags];
  if (layout) {
    return layout;
  }
  if (!this->iterDropFn) {
    auto *type = llvm::FunctionType::get(this->lowering.builder_.getVoidTy(),
                                         {this->lowering.builder_.getPtrTy()}, false);
    this->iterDropFn = llvm::Function::Create(type, llvm::GlobalValue::InternalLinkage,
                                              "tz.iter.destroy", this->lowering.module_.get());
    this->iterDropFn->addFnAttr(llvm::Attribute::NoUnwind);
    llvm::IRBuilder<> b(llvm::BasicBlock::Create(this->lowering.ctx_, "entry", this->iterDropFn));
    llvm::Value *frame = b.CreateConstInBoundsGEP1_64(b.getInt8Ty(), this->iterDropFn->getArg(0),
                                                      kHeaderSize, "frame");
    b.CreateCall(this->lowering.coroutineLowerer().coroIntrinsic(llvm::Intrinsic::coro_destroy),
                 {frame});
    b.CreateRetVoid();
  }
  auto *global = new llvm::GlobalVariable(
      *this->lowering.module_, this->lowering.layoutDescTy_, true,
      llvm::GlobalValue::PrivateLinkage,
      llvm::ConstantStruct::get(
          this->lowering.layoutDescTy_,
          {this->iterDropFn, llvm::ConstantPointerNull::get(this->lowering.builder_.getPtrTy()),
           this->lowering.builder_.getInt32(0), this->lowering.builder_.getInt32(flags),
           llvm::ConstantPointerNull::get(this->lowering.builder_.getPtrTy()),
           this->lowering.builder_.getInt32(0), this->lowering.builder_.getInt32(0)}),
      "tz.layout.iterator");
  global->setUnnamedAddr(llvm::GlobalValue::UnnamedAddr::Global);
  layout = global;

  return layout;
}

void GeneratorLowerer::emitGenerator(FunctionDecl *fn, llvm::Function *function) {
  if (fn->isAsync) {
    this->lowering.asyncGeneratorLowerer().emitAsyncGenerator(fn, function);
    return;
  }
  this->lowering.functionBuilder().beginFunction(function, fn);
  Lowering::Coroutine coroutine{};
  this->lowering.coro_ = &coroutine;
  const Type *type = fn->sym->ret;
  this->lowering.coroutineLowerer().beginCoroutine(
      kLayoutGenerator, type->isIterator() && !type->sent()->isVoid() ? type->sent() : nullptr);
  coroutine.yielded = type->isIterator() ? type->yielded() : nullptr;
  this->lowering.functionFrame().pushScope();
  std::vector<llvm::Value *> args;
  unsigned first = 0;
  if (fn->isClosure() || fn->takesThis()) {
    this->lowering.envParam_ = this->lowering.function_->getArg(0);
    if (fn->isClosure()) {
      this->lowering.envLowerer().bindCaptures(fn);
    } else {
      this->lowering.envParam_->setName("this");
    }
    llvm::Value *slot = this->lowering.functionFrame().createObjectSlot("env");
    this->lowering.builder_.CreateCall(this->lowering.objRetainFn_, {this->lowering.envParam_});
    this->lowering.builder_.CreateStore(this->lowering.envParam_, slot);
    first = 1;
  }
  for (unsigned i = first; i < this->lowering.function_->arg_size(); ++i) {
    args.push_back(this->lowering.function_->getArg(i));
  }
  this->lowering.functionBuilder().emitParams(fn, args);
  this->lowering.coroutineLowerer().emitSuspend();
  this->lowering.stmtLowerer().emitStmt(fn->body);
  if (this->lowering.functionFrame().blockIsOpen()) {
    this->lowering.coroutineLowerer().emitCoroutineReturn(nullptr, nullptr);
  }
  this->lowering.scopes_.pop_back();
  this->lowering.coroutineLowerer().finishCoroutine();
  this->lowering.coro_ = nullptr;
  this->lowering.function_ = nullptr;
}

llvm::Function *GeneratorLowerer::iteratorFunction(Builtin builtin, const Type *source,
                                                   const Type *operand, const Type *result) {
  llvm::Function *&fn = this->iteratorFunctions[{builtin, source, operand, result}];
  if (fn) {
    return fn;
  }
  std::vector<llvm::Type *> params{this->lowering.moduleBuilder().lower(source)};
  if (operand) {
    params.push_back(this->lowering.moduleBuilder().lower(operand));
  }
  if (builtin == Builtin::StrMatchAll) {
    params.push_back(this->lowering.builder_.getPtrTy());
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
  fn = llvm::Function::Create(
      llvm::FunctionType::get(this->lowering.builder_.getPtrTy(), params, false),
      llvm::GlobalValue::InternalLinkage, name, this->lowering.module_.get());
  this->lowering.functionBuilder().setFunctionAttributes(fn);
  this->lowering.pendingIterators_.push_back({fn, builtin, source, operand, result});

  return fn;
}

void GeneratorLowerer::emitPendingIterators() {
  while (!this->lowering.pendingIterators_.empty()) {
    Lowering::IteratorJob job = this->lowering.pendingIterators_.back();
    this->lowering.pendingIterators_.pop_back();
    this->lowering.functionBuilder().beginFunction(job.function, nullptr);
    Lowering::Coroutine coroutine{};
    this->lowering.coro_ = &coroutine;
    bool stage = isIteratorStage(job.builtin);
    bool matches = job.builtin == Builtin::StrMatchAll;
    bool dict = isDictIterator(job.builtin);
    llvm::StructType *dictPromise = nullptr;
    unsigned flags = 0;
    if (dict) {
      llvm::Type *ptr = this->lowering.builder_.getPtrTy();
      llvm::Type *i32 = this->lowering.builder_.getInt32Ty();
      dictPromise =
          llvm::StructType::get(this->lowering.ctx_, {ptr, ptr, ptr, i32, i32, ptr, i32, i32});
      flags = kLayoutDictIterator | (job.builtin == Builtin::MapValues ? kLayoutDictValues : 0) |
              (job.builtin == Builtin::MapEntries || job.builtin == Builtin::SetEntries
                   ? kLayoutDictEntries
                   : 0) |
              (job.source->isSet() ? kLayoutDictOfSet : 0);
    }
    this->lowering.coroutineLowerer().beginCoroutine(stage     ? kLayoutHelper
                                                     : matches ? kLayoutRegexIterator
                                                     : dict    ? flags
                                                     : job.source->isString()
                                                         ? kLayoutStringIterator
                                                         : kLayoutArrayIterator,
                                                     nullptr, dictPromise);
    this->lowering.functionFrame().pushScope();
    auto hold = [&](llvm::Value *value, const Type *type, const char *name) {
      llvm::Value *slot = type->isManaged()
                              ? this->lowering.functionFrame().createManagedSlot(type, name)
                              : this->lowering.functionFrame().createStackSlot(
                                    this->lowering.moduleBuilder().lower(type), name);
      this->lowering.builder_.CreateStore(value, slot);

      return slot;
    };
    llvm::Value *source = hold(this->lowering.function_->getArg(0), job.source, "source");
    llvm::Value *operand =
        job.operand ? hold(this->lowering.function_->getArg(1), job.operand, "operand") : nullptr;
    llvm::Value *lastSlot = nullptr;
    if (matches) {
      lastSlot = this->lowering.functionFrame().createStackSlot(
          this->lowering.builder_.getInt32Ty(), "lastIndex");
      llvm::Value *regex =
          this->lowering.builder_.CreateLoad(this->lowering.builder_.getPtrTy(), operand, "regex");
      this->lowering.builder_.CreateStore(
          this->lowering.builder_.CreateCall(
              this->lowering.regexLowerer().regexFunction("tz_re_match_all_begin",
                                                          this->lowering.builder_.getInt32Ty(),
                                                          {this->lowering.builder_.getPtrTy()}),
              {regex}),
          lastSlot);
    }
    llvm::Value *mapField = nullptr;
    if (dict) {
      mapField = this->lowering.builder_.CreateStructGEP(dictPromise, coroutine.promise, 5);
      this->lowering.builder_.CreateStore(
          this->lowering.builder_.CreateLoad(this->lowering.builder_.getPtrTy(), source), mapField);
    }
    this->lowering.coroutineLowerer().emitSuspend();
    Lowering::Consumer yield;
    yield.each = [&](llvm::Value *value) {
      return this->lowering.coroutineLowerer().emitCoroutineYield(value, nullptr);
    };
    yield.stop = [&] {
      this->lowering.coroutineLowerer().emitCoroutineReturn(nullptr, nullptr);
      this->lowering.builder_.CreateUnreachable();
    };
    if (matches) {
      this->lowering.regexLowerer()
          .matchAllProducer(source, operand, lastSlot, this->lowering.function_->getArg(2))
          .run(yield);
    } else if (dict) {
      this->lowering.dictLowerer().beginDictIteration(source);
      llvm::Value *position =
          this->lowering.builder_.CreateStructGEP(dictPromise, coroutine.promise, 6);
      this->lowering.dictLowerer()
          .dictProducer(mapField, job.source, job.builtin, position)
          .run(yield);
      if (this->lowering.functionFrame().blockIsOpen()) {
        this->lowering.builder_.CreateStore(
            llvm::ConstantPointerNull::get(this->lowering.builder_.getPtrTy()), mapField);
      }
    } else if (!stage) {
      Lowering::Producer producer =
          job.source->isArray()
              ? this->lowering.lazyChainLowerer().arrayProducer(source, job.source, job.builtin)
              : this->lowering.lazyChainLowerer().valueProducer(source, job.source);
      producer.run(yield);
    } else {
      const Type *callback = job.operand->isFunction() ? job.operand : nullptr;
      this->lowering.lazyChainLowerer()
          .valueProducer(source, job.source)
          .run(this->lowering.lazyChainLowerer().stageConsumer(
              job.builtin, callback, job.source->yielded(), job.result->yielded(), operand, yield));
    }
    if (this->lowering.functionFrame().blockIsOpen()) {
      this->lowering.coroutineLowerer().emitCoroutineReturn(nullptr, nullptr);
    }
    this->lowering.scopes_.pop_back();
    this->lowering.coroutineLowerer().finishCoroutine();
    this->lowering.coro_ = nullptr;
    this->lowering.function_ = nullptr;
  }
}

}
