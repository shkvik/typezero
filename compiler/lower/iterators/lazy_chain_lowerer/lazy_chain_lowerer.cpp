#include "compiler/lower/iterators/lazy_chain_lowerer/lazy_chain_lowerer.h"
#include "compiler/check/checker/checker.h"
#include "compiler/library/bytes/bytes_lowerer/bytes_lowerer.h"
#include "compiler/library/process/process_lowerer/process_lowerer.h"
#include "compiler/lower/errors/throw_lowerer/throw_lowerer.h"
#include "compiler/lower/expr/expr_lowerer/expr_lowerer.h"
#include "compiler/lower/function_frame/function_frame.h"
#include "compiler/lower/functions/closure_lowerer/closure_lowerer.h"
#include "compiler/lower/iterators/coroutine_lowerer/coroutine_lowerer.h"
#include "compiler/lower/layouts/object_layouts/object_layouts.h"
#include "compiler/lower/memory/group_lowerer/group_lowerer.h"
#include "compiler/lower/memory/ref_counts/ref_counts.h"
#include "compiler/lower/module_builder/module_builder.h"
#include "compiler/lower/values/array_lowerer/array_lowerer.h"
#include "compiler/lower/values/string_lowerer/string_lowerer.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/IR/MDBuilder.h"
#include "llvm/Support/ErrorHandling.h"

using llvm::BasicBlock;
using llvm::cast;
using llvm::dyn_cast;
using llvm::isa;
using llvm::Value;

namespace tz {

LazyChainLowerer::LazyChainLowerer(Lowering &lowering) : lowering_(lowering) {}

llvm::Value *LazyChainLowerer::callCallback(llvm::Value *slot, const Type *fnType,
                                            std::vector<llvm::Value *> args,
                                            llvm::ArrayRef<const Type *> types) {
  size_t passes = std::min(fnType->params().size(), args.size());
  for (size_t i = passes; i < args.size(); ++i) {
    lowering_.refCounts().releaseValue(args[i], types[i]);
  }
  args.resize(passes);
  llvm::Value *fn = lowering_.builder_.CreateLoad(lowering_.fnTy_, slot, "callback");
  llvm::Value *result = lowering_.closureLowerer().callFunctionValue(fn, fnType, args);
  lowering_.emitThrowCheckIf(lowering_.typeThrows(fnType));

  return result;
}

Lowering::Producer LazyChainLowerer::arrayProducer(llvm::Value *slot, const Type *arrayType,
                                                   Builtin mode) {
  Lowering::Producer producer;
  producer.run = [this, slot, arrayType, mode](const Lowering::Consumer &consumer) {
    const Type *element = arrayType->inner();
    llvm::Type *i64 = lowering_.builder_.getInt64Ty();
    // As JavaScript's iterator, each step reads the current length, so
    // elements pushed meanwhile are visited too.
    llvm::Value *indexSlot = lowering_.functionFrame().createStackSlot(i64, "index");
    lowering_.builder_.CreateStore(lowering_.builder_.getInt64(0), indexSlot);
    llvm::BasicBlock *headBlock = lowering_.functionFrame().newBlock("iter.array");
    llvm::BasicBlock *bodyBlock = lowering_.functionFrame().newBlock("iter.array.body");
    llvm::BasicBlock *endBlock = lowering_.functionFrame().newBlock("iter.array.end");
    lowering_.functionFrame().emitBlock(headBlock);
    llvm::Value *array =
        lowering_.builder_.CreateLoad(lowering_.builder_.getPtrTy(), slot, "array");
    llvm::Value *index = lowering_.builder_.CreateLoad(i64, indexSlot, "index");
    lowering_.builder_.CreateCondBr(
        lowering_.builder_.CreateICmpULT(index, lowering_.arrayLowerer().arrayLength(array)),
        bodyBlock, endBlock);
    lowering_.functionFrame().switchTo(bodyBlock);
    lowering_.builder_.CreateStore(
        lowering_.builder_.CreateAdd(index, lowering_.builder_.getInt64(1)), indexSlot);
    llvm::Value *value;
    if (mode == Builtin::ArrKeys) {
      value = lowering_.builder_.CreateTrunc(index, lowering_.builder_.getInt32Ty());
    } else {
      value = lowering_.builder_.CreateLoad(
          lowering_.moduleBuilder().lower(element),
          lowering_.arrayLowerer().elementAddress(lowering_.arrayLowerer().arrayData(array),
                                                  element, index),
          "element");
      lowering_.refCounts().retainValue(value, element);
      if (mode == Builtin::ArrEntries) {
        // A new [index, value] each time, as in JavaScript.
        const Type *tuple = Type::tuple({Type::intTy(), element});
        llvm::Value *entry = lowering_.objectLayouts().allocObject(tuple, {0, 1});
        lowering_.builder_.CreateStore(
            lowering_.builder_.CreateTrunc(index, lowering_.builder_.getInt32Ty()),
            lowering_.objectLayouts().fieldAddress(entry, tuple, "0"));
        lowering_.builder_.CreateStore(value,
                                       lowering_.objectLayouts().fieldAddress(entry, tuple, "1"));
        lowering_.groupLowerer().adoptFields(entry, tuple);
        value = entry;
      }
    }
    if (llvm::Value *sent = consumer.each(value)) {
      lowering_.refCounts().releaseValue(sent, consumer.sent);
    }
    lowering_.functionFrame().branchTo(headBlock);
    lowering_.functionFrame().switchTo(endBlock);
  };

  return producer;
}

Lowering::Producer LazyChainLowerer::valueProducer(llvm::Value *slot, const Type *type,
                                                   llvm::Value *returnSlot) {
  if (type->isArray()) {
    return arrayProducer(slot, type, Builtin::ArrValues);
  }
  if (type->isTypedArray()) {
    return lowering_.bytesLowerer().bytesProducer(slot, type);
  }
  Lowering::Producer producer;
  if (type->isString()) {
    producer.run = [this, slot](const Lowering::Consumer &consumer) {
      llvm::Type *i64 = lowering_.builder_.getInt64Ty();
      llvm::Value *indexSlot = lowering_.functionFrame().createStackSlot(i64, "index");
      lowering_.builder_.CreateStore(lowering_.builder_.getInt64(0), indexSlot);
      llvm::BasicBlock *headBlock = lowering_.functionFrame().newBlock("iter.string");
      llvm::BasicBlock *bodyBlock = lowering_.functionFrame().newBlock("iter.string.body");
      llvm::BasicBlock *endBlock = lowering_.functionFrame().newBlock("iter.string.end");
      lowering_.functionFrame().emitBlock(headBlock);
      llvm::Value *str = lowering_.builder_.CreateLoad(lowering_.strTy_, slot, "string");
      llvm::Value *index = lowering_.builder_.CreateLoad(i64, indexSlot, "index");
      lowering_.builder_.CreateCondBr(
          lowering_.builder_.CreateICmpULT(index, lowering_.stringLowerer().stringLength(str)),
          bodyBlock, endBlock);
      lowering_.functionFrame().switchTo(bodyBlock);
      // One code point: one code unit, or two for a surrogate pair.
      llvm::Value *out = lowering_.functionFrame().createStackSlot(lowering_.strTy_, "code.point");
      llvm::Value *units = lowering_.builder_.CreateCall(
          lowering_.rt_.strCodePointAt, {out, lowering_.stringLowerer().stringObj(str),
                                         lowering_.stringLowerer().stringMeta(str), index});
      lowering_.builder_.CreateStore(
          lowering_.builder_.CreateAdd(index, lowering_.builder_.CreateSExt(units, i64)),
          indexSlot);
      if (llvm::Value *sent = consumer.each(lowering_.builder_.CreateLoad(lowering_.strTy_, out))) {
        lowering_.refCounts().releaseValue(sent, consumer.sent);
      }
      lowering_.functionFrame().branchTo(headBlock);
      lowering_.functionFrame().switchTo(endBlock);
    };
    return producer;
  }
  // An iterator object: next() until it is done; what a generator returns
  // goes to `returnSlot`, if anywhere.
  producer.run = [this, slot, type, returnSlot](const Lowering::Consumer &consumer) {
    const Type *element = type->yielded();
    llvm::Value *out = lowering_.functionFrame().createStackSlot(
        lowering_.moduleBuilder().lower(element), "next.value");
    // What the consumer gives back goes to the next call of next(), as
    // `yield*` passes on what is sent to it.
    llvm::Value *sentSlot = nullptr, *hasSent = nullptr;
    if (consumer.sent) {
      sentSlot = lowering_.functionFrame().createStackSlot(
          lowering_.moduleBuilder().lower(consumer.sent), "next.sent");
      hasSent = lowering_.functionFrame().createStackSlot(lowering_.builder_.getInt1Ty(),
                                                          "next.has.sent");
      lowering_.builder_.CreateStore(lowering_.builder_.getFalse(), hasSent);
    }
    llvm::BasicBlock *headBlock = lowering_.functionFrame().newBlock("iter.next");
    llvm::BasicBlock *bodyBlock = lowering_.functionFrame().newBlock("iter.next.body");
    llvm::BasicBlock *endBlock = lowering_.functionFrame().newBlock("iter.next.end");
    lowering_.functionFrame().emitBlock(headBlock);
    llvm::Value *it =
        lowering_.builder_.CreateLoad(lowering_.builder_.getPtrTy(), slot, "iterator");
    llvm::Value *in = nullptr;
    if (hasSent) {
      in = lowering_.builder_.CreateSelect(
          lowering_.builder_.CreateLoad(lowering_.builder_.getInt1Ty(), hasSent), sentSlot,
          llvm::ConstantPointerNull::get(lowering_.builder_.getPtrTy()));
    }
    llvm::Value *has = lowering_.coroutineLowerer().emitResume(it, out, in, returnSlot);
    if (hasSent) {
      // The generator took a copy of what it needed.
      llvm::BasicBlock *releaseBlock = lowering_.functionFrame().newBlock("next.release");
      llvm::BasicBlock *afterBlock = lowering_.functionFrame().newBlock("next.after");
      lowering_.builder_.CreateCondBr(
          lowering_.builder_.CreateLoad(lowering_.builder_.getInt1Ty(), hasSent), releaseBlock,
          afterBlock);
      lowering_.functionFrame().switchTo(releaseBlock);
      lowering_.refCounts().releaseValue(
          lowering_.builder_.CreateLoad(lowering_.moduleBuilder().lower(consumer.sent), sentSlot),
          consumer.sent);
      lowering_.builder_.CreateStore(lowering_.builder_.getFalse(), hasSent);
      lowering_.builder_.CreateBr(afterBlock);
      lowering_.functionFrame().switchTo(afterBlock);
    }
    lowering_.builder_.CreateCondBr(has, bodyBlock, endBlock);
    lowering_.functionFrame().switchTo(bodyBlock);
    llvm::Value *sent = consumer.each(
        lowering_.builder_.CreateLoad(lowering_.moduleBuilder().lower(element), out, "value"));
    if (sent && lowering_.functionFrame().blockIsOpen()) {
      lowering_.builder_.CreateStore(sent, sentSlot);
      lowering_.builder_.CreateStore(lowering_.builder_.getTrue(), hasSent);
    }
    lowering_.functionFrame().branchTo(headBlock);
    lowering_.functionFrame().switchTo(endBlock);
  };

  return producer;
}

// The stages of a chain take each value from the stage before them and give
// what they make of it to the next one.
Lowering::Consumer LazyChainLowerer::stageConsumer(Builtin stage, const Type *callbackType,
                                                   const Type *element, const Type *result,
                                                   llvm::Value *operand,
                                                   const Lowering::Consumer &down) {
  llvm::Type *i32 = lowering_.builder_.getInt32Ty(), *i64 = lowering_.builder_.getInt64Ty();
  Lowering::Consumer stageOut;
  stageOut.stop = down.stop;
  // What the next stage gives back is not passed on: helpers call next()
  // without a value.
  auto give = [this, down](llvm::Value *value) {
    if (llvm::Value *sent = down.each(value)) {
      lowering_.refCounts().releaseValue(sent, down.sent);
    }
  };
  // The counter callbacks get.
  llvm::Value *counter = nullptr;
  if (callbackType) {
    counter = lowering_.functionFrame().createStackSlot(i32, "counter");
    lowering_.builder_.CreateStore(lowering_.builder_.getInt32(0), counter);
  }
  auto nextCount = [this, counter, i32] {
    llvm::Value *count = lowering_.builder_.CreateLoad(i32, counter, "count");
    lowering_.builder_.CreateStore(
        lowering_.builder_.CreateAdd(count, lowering_.builder_.getInt32(1)), counter);

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
      lowering_.refCounts().retainValue(value, element); // the callback owns what it gets
      llvm::Value *keep =
          callCallback(operand, callbackType, {value, nextCount()}, {element, Type::intTy()});
      llvm::BasicBlock *keepBlock = lowering_.functionFrame().newBlock("filter.keep");
      llvm::BasicBlock *dropBlock = lowering_.functionFrame().newBlock("filter.drop");
      llvm::BasicBlock *nextBlock = lowering_.functionFrame().newBlock("filter.next");
      lowering_.builder_.CreateCondBr(keep, keepBlock, dropBlock);
      lowering_.functionFrame().switchTo(keepBlock);
      // `filter(x => x != undefined)` gives the values themselves.
      give(lowering_.exprLowerer().emitConversion(value, element, result));
      lowering_.functionFrame().branchTo(nextBlock);
      lowering_.functionFrame().switchTo(dropBlock);
      lowering_.refCounts().releaseValue(value, element);
      lowering_.builder_.CreateBr(nextBlock);
      lowering_.functionFrame().switchTo(nextBlock);

      return nullptr;
    };
    break;
  case Builtin::IterTake:
  case Builtin::IterDrop: {
    // The count left; a negative one is a RangeError, as in JavaScript.
    llvm::Value *left = lowering_.functionFrame().createStackSlot(i64, "left");
    llvm::Value *count = lowering_.builder_.CreateLoad(i64, operand, "limit");
    llvm::BasicBlock *badBlock = lowering_.functionFrame().newBlock("limit.negative");
    llvm::BasicBlock *okBlock = lowering_.functionFrame().newBlock("limit.ok");
    lowering_.builder_.CreateCondBr(
        lowering_.builder_.CreateICmpSLT(count, lowering_.builder_.getInt64(0)), badBlock, okBlock,
        llvm::MDBuilder(lowering_.ctx_).createUnlikelyBranchWeights());
    lowering_.functionFrame().switchTo(badBlock);
    lowering_.builder_.CreateCall(lowering_.processLowerer().processFunction(
                                      "tz_iter_bad_count", lowering_.builder_.getVoidTy(), {i64}),
                                  {count});
    lowering_.throwLowerer().emitRuntimeThrow();
    lowering_.functionFrame().switchTo(okBlock);
    lowering_.builder_.CreateStore(count, left);
    if (stage == Builtin::IterTake) {
      // take(0) gives nothing and asks for nothing.
      llvm::BasicBlock *zeroBlock = lowering_.functionFrame().newBlock("take.none");
      llvm::BasicBlock *startBlock = lowering_.functionFrame().newBlock("take.start");
      lowering_.builder_.CreateCondBr(
          lowering_.builder_.CreateICmpEQ(count, lowering_.builder_.getInt64(0)), zeroBlock,
          startBlock);
      lowering_.functionFrame().switchTo(zeroBlock);
      down.stop();
      lowering_.functionFrame().switchTo(startBlock);
      stageOut.each = [=, this](llvm::Value *value) -> llvm::Value * {
        llvm::Value *rest = lowering_.builder_.CreateSub(lowering_.builder_.CreateLoad(i64, left),
                                                         lowering_.builder_.getInt64(1));
        lowering_.builder_.CreateStore(rest, left);
        give(value);
        if (!lowering_.functionFrame().blockIsOpen()) {
          return nullptr;
        }
        // After the last one, the iteration ends without asking for more.
        llvm::BasicBlock *doneBlock = lowering_.functionFrame().newBlock("take.done");
        llvm::BasicBlock *moreBlock = lowering_.functionFrame().newBlock("take.more");
        lowering_.builder_.CreateCondBr(
            lowering_.builder_.CreateICmpEQ(rest, lowering_.builder_.getInt64(0)), doneBlock,
            moreBlock);
        lowering_.functionFrame().switchTo(doneBlock);
        down.stop();
        lowering_.functionFrame().switchTo(moreBlock);

        return nullptr;
      };
    } else {
      stageOut.each = [=, this](llvm::Value *value) -> llvm::Value * {
        llvm::Value *rest = lowering_.builder_.CreateLoad(i64, left);
        llvm::BasicBlock *skipBlock = lowering_.functionFrame().newBlock("drop.skip");
        llvm::BasicBlock *passBlock = lowering_.functionFrame().newBlock("drop.pass");
        llvm::BasicBlock *nextBlock = lowering_.functionFrame().newBlock("drop.next");
        lowering_.builder_.CreateCondBr(
            lowering_.builder_.CreateICmpSGT(rest, lowering_.builder_.getInt64(0)), skipBlock,
            passBlock);
        lowering_.functionFrame().switchTo(skipBlock);
        lowering_.builder_.CreateStore(
            lowering_.builder_.CreateSub(rest, lowering_.builder_.getInt64(1)), left);
        lowering_.refCounts().releaseValue(value, element);
        lowering_.builder_.CreateBr(nextBlock);
        lowering_.functionFrame().switchTo(passBlock);
        give(value);
        lowering_.functionFrame().branchTo(nextBlock);
        lowering_.functionFrame().switchTo(nextBlock);

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
      lowering_.functionFrame().pushScope();
      llvm::Value *slot = lowering_.functionFrame().createManagedSlot(inner, "flat");
      lowering_.builder_.CreateStore(iterable, slot);
      Lowering::Consumer flat;
      flat.each = [=](llvm::Value *v) -> llvm::Value * {
        give(v);

        return nullptr;
      };
      flat.stop = down.stop;
      valueProducer(slot, inner).run(flat);
      lowering_.functionFrame().popScope();

      return nullptr;
    };
    break;
  default:
    llvm_unreachable("not a stage of a chain");
  }

  return stageOut;
}

} // namespace tz
