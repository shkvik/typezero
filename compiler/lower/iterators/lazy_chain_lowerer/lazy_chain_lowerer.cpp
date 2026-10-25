#include "compiler/lower/iterators/lazy_chain_lowerer/lazy_chain_lowerer.h"
#include "compiler/lower/module_builder/module_builder.h"
#include "compiler/lower/function_frame/function_frame.h"
#include "compiler/lower/memory/ref_counts/ref_counts.h"
#include "compiler/lower/expr/expr_lowerer/expr_lowerer.h"
#include "compiler/lower/values/string_lowerer/string_lowerer.h"
#include "compiler/lower/values/array_lowerer/array_lowerer.h"
#include "compiler/lower/functions/closure_lowerer/closure_lowerer.h"
#include "compiler/lower/memory/group_lowerer/group_lowerer.h"
#include "compiler/lower/iterators/coroutine_lowerer/coroutine_lowerer.h"
#include "compiler/lower/layouts/object_layouts/object_layouts.h"
#include "compiler/lower/errors/throw_lowerer/throw_lowerer.h"
#include "compiler/library/bytes/bytes_lowerer/bytes_lowerer.h"
#include "compiler/library/process/process_lowerer/process_lowerer.h"
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

LazyChainLowerer::LazyChainLowerer(CodeGen &codegen) : codegen_(codegen) {}

llvm::Value *LazyChainLowerer::callCallback(llvm::Value *slot, const Type *fnType,
                                   std::vector<llvm::Value *> args,
                                   llvm::ArrayRef<const Type *> types) {
  size_t passes = std::min(fnType->params().size(), args.size());
  for (size_t i = passes; i < args.size(); ++i) {
    codegen_.refCounts().releaseValue(args[i], types[i]);
  }
  args.resize(passes);
  llvm::Value *fn = codegen_.builder_.CreateLoad(codegen_.fnTy_, slot, "callback");
  llvm::Value *result = codegen_.closureLowerer().callFunctionValue(fn, fnType, args);
  codegen_.emitThrowCheckIf(codegen_.typeThrows(fnType));

  return result;
}

CodeGen::Producer LazyChainLowerer::arrayProducer(llvm::Value *slot, const Type *arrayType, Builtin mode) {
  CodeGen::Producer producer;
  producer.run = [this, slot, arrayType, mode](const CodeGen::Consumer &consumer) {
    const Type *element = arrayType->inner();
    llvm::Type *i64 = codegen_.builder_.getInt64Ty();
    // As JavaScript's iterator, each step reads the current length, so
    // elements pushed meanwhile are visited too.
    llvm::Value *indexSlot = codegen_.functionFrame().createStackSlot(i64, "index");
    codegen_.builder_.CreateStore(codegen_.builder_.getInt64(0), indexSlot);
    llvm::BasicBlock *headBlock = codegen_.functionFrame().newBlock("iter.array");
    llvm::BasicBlock *bodyBlock = codegen_.functionFrame().newBlock("iter.array.body");
    llvm::BasicBlock *endBlock = codegen_.functionFrame().newBlock("iter.array.end");
    codegen_.functionFrame().emitBlock(headBlock);
    llvm::Value *array = codegen_.builder_.CreateLoad(codegen_.builder_.getPtrTy(), slot, "array");
    llvm::Value *index = codegen_.builder_.CreateLoad(i64, indexSlot, "index");
    codegen_.builder_.CreateCondBr(codegen_.builder_.CreateICmpULT(index, codegen_.arrayLowerer().arrayLength(array)), bodyBlock, endBlock);
    codegen_.functionFrame().switchTo(bodyBlock);
    codegen_.builder_.CreateStore(codegen_.builder_.CreateAdd(index, codegen_.builder_.getInt64(1)), indexSlot);
    llvm::Value *value;
    if (mode == Builtin::ArrKeys) {
      value = codegen_.builder_.CreateTrunc(index, codegen_.builder_.getInt32Ty());
    } else {
      value = codegen_.builder_.CreateLoad(codegen_.moduleBuilder().lower(element), codegen_.arrayLowerer().elementAddress(codegen_.arrayLowerer().arrayData(array), element, index),
                                  "element");
      codegen_.refCounts().retainValue(value, element);
      if (mode == Builtin::ArrEntries) {
        // A new [index, value] each time, as in JavaScript.
        const Type *tuple = Type::tuple({Type::intTy(), element});
        llvm::Value *entry = codegen_.objectLayouts().allocObject(tuple, {0, 1});
        codegen_.builder_.CreateStore(codegen_.builder_.CreateTrunc(index, codegen_.builder_.getInt32Ty()),
                             codegen_.objectLayouts().fieldAddress(entry, tuple, "0"));
        codegen_.builder_.CreateStore(value, codegen_.objectLayouts().fieldAddress(entry, tuple, "1"));
        codegen_.groupLowerer().adoptFields(entry, tuple);
        value = entry;
      }
    }
    if (llvm::Value *sent = consumer.each(value)) {
      codegen_.refCounts().releaseValue(sent, consumer.sent);
    }
    codegen_.functionFrame().branchTo(headBlock);
    codegen_.functionFrame().switchTo(endBlock);
  };

  return producer;
}

CodeGen::Producer LazyChainLowerer::valueProducer(llvm::Value *slot, const Type *type,
                                         llvm::Value *returnSlot) {
  if (type->isArray()) {
    return arrayProducer(slot, type, Builtin::ArrValues);
  }
  if (type->isTypedArray()) {
    return codegen_.bytesLowerer().bytesProducer(slot, type);
  }
  CodeGen::Producer producer;
  if (type->isString()) {
    producer.run = [this, slot](const CodeGen::Consumer &consumer) {
      llvm::Type *i64 = codegen_.builder_.getInt64Ty();
      llvm::Value *indexSlot = codegen_.functionFrame().createStackSlot(i64, "index");
      codegen_.builder_.CreateStore(codegen_.builder_.getInt64(0), indexSlot);
      llvm::BasicBlock *headBlock = codegen_.functionFrame().newBlock("iter.string");
      llvm::BasicBlock *bodyBlock = codegen_.functionFrame().newBlock("iter.string.body");
      llvm::BasicBlock *endBlock = codegen_.functionFrame().newBlock("iter.string.end");
      codegen_.functionFrame().emitBlock(headBlock);
      llvm::Value *str = codegen_.builder_.CreateLoad(codegen_.strTy_, slot, "string");
      llvm::Value *index = codegen_.builder_.CreateLoad(i64, indexSlot, "index");
      codegen_.builder_.CreateCondBr(codegen_.builder_.CreateICmpULT(index, codegen_.stringLowerer().stringLength(str)), bodyBlock, endBlock);
      codegen_.functionFrame().switchTo(bodyBlock);
      // One code point: one code unit, or two for a surrogate pair.
      llvm::Value *out = codegen_.functionFrame().createStackSlot(codegen_.strTy_, "code.point");
      llvm::Value *units =
          codegen_.builder_.CreateCall(codegen_.rt_.strCodePointAt, {out, codegen_.stringLowerer().stringObj(str), codegen_.stringLowerer().stringMeta(str), index});
      codegen_.builder_.CreateStore(codegen_.builder_.CreateAdd(index, codegen_.builder_.CreateSExt(units, i64)), indexSlot);
      if (llvm::Value *sent = consumer.each(codegen_.builder_.CreateLoad(codegen_.strTy_, out))) {
        codegen_.refCounts().releaseValue(sent, consumer.sent);
      }
      codegen_.functionFrame().branchTo(headBlock);
      codegen_.functionFrame().switchTo(endBlock);
    };
    return producer;
  }
  // An iterator object: next() until it is done; what a generator returns
  // goes to `returnSlot`, if anywhere.
  producer.run = [this, slot, type, returnSlot](const CodeGen::Consumer &consumer) {
    const Type *element = type->yielded();
    llvm::Value *out = codegen_.functionFrame().createStackSlot(codegen_.moduleBuilder().lower(element), "next.value");
    // What the consumer gives back goes to the next call of next(), as
    // `yield*` passes on what is sent to it.
    llvm::Value *sentSlot = nullptr, *hasSent = nullptr;
    if (consumer.sent) {
      sentSlot = codegen_.functionFrame().createStackSlot(codegen_.moduleBuilder().lower(consumer.sent), "next.sent");
      hasSent = codegen_.functionFrame().createStackSlot(codegen_.builder_.getInt1Ty(), "next.has.sent");
      codegen_.builder_.CreateStore(codegen_.builder_.getFalse(), hasSent);
    }
    llvm::BasicBlock *headBlock = codegen_.functionFrame().newBlock("iter.next");
    llvm::BasicBlock *bodyBlock = codegen_.functionFrame().newBlock("iter.next.body");
    llvm::BasicBlock *endBlock = codegen_.functionFrame().newBlock("iter.next.end");
    codegen_.functionFrame().emitBlock(headBlock);
    llvm::Value *it = codegen_.builder_.CreateLoad(codegen_.builder_.getPtrTy(), slot, "iterator");
    llvm::Value *in = nullptr;
    if (hasSent) {
      in = codegen_.builder_.CreateSelect(codegen_.builder_.CreateLoad(codegen_.builder_.getInt1Ty(), hasSent), sentSlot,
                                 llvm::ConstantPointerNull::get(codegen_.builder_.getPtrTy()));
    }
    llvm::Value *has = codegen_.coroutineLowerer().emitResume(it, out, in, returnSlot);
    if (hasSent) {
      // The generator took a copy of what it needed.
      llvm::BasicBlock *releaseBlock = codegen_.functionFrame().newBlock("next.release");
      llvm::BasicBlock *afterBlock = codegen_.functionFrame().newBlock("next.after");
      codegen_.builder_.CreateCondBr(codegen_.builder_.CreateLoad(codegen_.builder_.getInt1Ty(), hasSent), releaseBlock,
                            afterBlock);
      codegen_.functionFrame().switchTo(releaseBlock);
      codegen_.refCounts().releaseValue(codegen_.builder_.CreateLoad(codegen_.moduleBuilder().lower(consumer.sent), sentSlot), consumer.sent);
      codegen_.builder_.CreateStore(codegen_.builder_.getFalse(), hasSent);
      codegen_.builder_.CreateBr(afterBlock);
      codegen_.functionFrame().switchTo(afterBlock);
    }
    codegen_.builder_.CreateCondBr(has, bodyBlock, endBlock);
    codegen_.functionFrame().switchTo(bodyBlock);
    llvm::Value *sent = consumer.each(codegen_.builder_.CreateLoad(codegen_.moduleBuilder().lower(element), out, "value"));
    if (sent && codegen_.functionFrame().blockIsOpen()) {
      codegen_.builder_.CreateStore(sent, sentSlot);
      codegen_.builder_.CreateStore(codegen_.builder_.getTrue(), hasSent);
    }
    codegen_.functionFrame().branchTo(headBlock);
    codegen_.functionFrame().switchTo(endBlock);
  };

  return producer;
}

// The stages of a chain take each value from the stage before them and give
// what they make of it to the next one.
CodeGen::Consumer LazyChainLowerer::stageConsumer(Builtin stage, const Type *callbackType,
                                         const Type *element, const Type *result,
                                         llvm::Value *operand, const CodeGen::Consumer &down) {
  llvm::Type *i32 = codegen_.builder_.getInt32Ty(), *i64 = codegen_.builder_.getInt64Ty();
  CodeGen::Consumer stageOut;
  stageOut.stop = down.stop;
  // What the next stage gives back is not passed on: helpers call next()
  // without a value.
  auto give = [this, down](llvm::Value *value) {
    if (llvm::Value *sent = down.each(value)) {
      codegen_.refCounts().releaseValue(sent, down.sent);
    }
  };
  // The counter callbacks get.
  llvm::Value *counter = nullptr;
  if (callbackType) {
    counter = codegen_.functionFrame().createStackSlot(i32, "counter");
    codegen_.builder_.CreateStore(codegen_.builder_.getInt32(0), counter);
  }
  auto nextCount = [this, counter, i32] {
    llvm::Value *count = codegen_.builder_.CreateLoad(i32, counter, "count");
    codegen_.builder_.CreateStore(codegen_.builder_.CreateAdd(count, codegen_.builder_.getInt32(1)), counter);

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
      codegen_.refCounts().retainValue(value, element); // the callback owns what it gets
      llvm::Value *keep =
          callCallback(operand, callbackType, {value, nextCount()}, {element, Type::intTy()});
      llvm::BasicBlock *keepBlock = codegen_.functionFrame().newBlock("filter.keep");
      llvm::BasicBlock *dropBlock = codegen_.functionFrame().newBlock("filter.drop");
      llvm::BasicBlock *nextBlock = codegen_.functionFrame().newBlock("filter.next");
      codegen_.builder_.CreateCondBr(keep, keepBlock, dropBlock);
      codegen_.functionFrame().switchTo(keepBlock);
      // `filter(x => x != undefined)` gives the values themselves.
      give(codegen_.exprLowerer().emitConversion(value, element, result));
      codegen_.functionFrame().branchTo(nextBlock);
      codegen_.functionFrame().switchTo(dropBlock);
      codegen_.refCounts().releaseValue(value, element);
      codegen_.builder_.CreateBr(nextBlock);
      codegen_.functionFrame().switchTo(nextBlock);

      return nullptr;
    };
    break;
  case Builtin::IterTake:
  case Builtin::IterDrop: {
    // The count left; a negative one is a RangeError, as in JavaScript.
    llvm::Value *left = codegen_.functionFrame().createStackSlot(i64, "left");
    llvm::Value *count = codegen_.builder_.CreateLoad(i64, operand, "limit");
    llvm::BasicBlock *badBlock = codegen_.functionFrame().newBlock("limit.negative");
    llvm::BasicBlock *okBlock = codegen_.functionFrame().newBlock("limit.ok");
    codegen_.builder_.CreateCondBr(codegen_.builder_.CreateICmpSLT(count, codegen_.builder_.getInt64(0)), badBlock, okBlock,
                          llvm::MDBuilder(codegen_.ctx_).createUnlikelyBranchWeights());
    codegen_.functionFrame().switchTo(badBlock);
    codegen_.builder_.CreateCall(codegen_.processLowerer().processFunction("tz_iter_bad_count", codegen_.builder_.getVoidTy(), {i64}), {count});
    codegen_.throwLowerer().emitRuntimeThrow();
    codegen_.functionFrame().switchTo(okBlock);
    codegen_.builder_.CreateStore(count, left);
    if (stage == Builtin::IterTake) {
      // take(0) gives nothing and asks for nothing.
      llvm::BasicBlock *zeroBlock = codegen_.functionFrame().newBlock("take.none");
      llvm::BasicBlock *startBlock = codegen_.functionFrame().newBlock("take.start");
      codegen_.builder_.CreateCondBr(codegen_.builder_.CreateICmpEQ(count, codegen_.builder_.getInt64(0)), zeroBlock,
                            startBlock);
      codegen_.functionFrame().switchTo(zeroBlock);
      down.stop();
      codegen_.functionFrame().switchTo(startBlock);
      stageOut.each = [=, this](llvm::Value *value) -> llvm::Value * {
        llvm::Value *rest =
            codegen_.builder_.CreateSub(codegen_.builder_.CreateLoad(i64, left), codegen_.builder_.getInt64(1));
        codegen_.builder_.CreateStore(rest, left);
        give(value);
        if (!codegen_.functionFrame().blockIsOpen()) {
          return nullptr;
        }
        // After the last one, the iteration ends without asking for more.
        llvm::BasicBlock *doneBlock = codegen_.functionFrame().newBlock("take.done");
        llvm::BasicBlock *moreBlock = codegen_.functionFrame().newBlock("take.more");
        codegen_.builder_.CreateCondBr(codegen_.builder_.CreateICmpEQ(rest, codegen_.builder_.getInt64(0)), doneBlock,
                              moreBlock);
        codegen_.functionFrame().switchTo(doneBlock);
        down.stop();
        codegen_.functionFrame().switchTo(moreBlock);

        return nullptr;
      };
    } else {
      stageOut.each = [=, this](llvm::Value *value) -> llvm::Value * {
        llvm::Value *rest = codegen_.builder_.CreateLoad(i64, left);
        llvm::BasicBlock *skipBlock = codegen_.functionFrame().newBlock("drop.skip");
        llvm::BasicBlock *passBlock = codegen_.functionFrame().newBlock("drop.pass");
        llvm::BasicBlock *nextBlock = codegen_.functionFrame().newBlock("drop.next");
        codegen_.builder_.CreateCondBr(codegen_.builder_.CreateICmpSGT(rest, codegen_.builder_.getInt64(0)), skipBlock,
                              passBlock);
        codegen_.functionFrame().switchTo(skipBlock);
        codegen_.builder_.CreateStore(codegen_.builder_.CreateSub(rest, codegen_.builder_.getInt64(1)), left);
        codegen_.refCounts().releaseValue(value, element);
        codegen_.builder_.CreateBr(nextBlock);
        codegen_.functionFrame().switchTo(passBlock);
        give(value);
        codegen_.functionFrame().branchTo(nextBlock);
        codegen_.functionFrame().switchTo(nextBlock);

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
      codegen_.functionFrame().pushScope();
      llvm::Value *slot = codegen_.functionFrame().createManagedSlot(inner, "flat");
      codegen_.builder_.CreateStore(iterable, slot);
      CodeGen::Consumer flat;
      flat.each = [=](llvm::Value *v) -> llvm::Value * {
        give(v);

        return nullptr;
      };
      flat.stop = down.stop;
      valueProducer(slot, inner).run(flat);
      codegen_.functionFrame().popScope();

      return nullptr;
    };
    break;
  default:
    llvm_unreachable("not a stage of a chain");
  }

  return stageOut;
}

} // namespace tz
