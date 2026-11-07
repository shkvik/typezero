#include "compiler/lower/iterators/lazy_chain_lowerer/lazy_chain_lowerer.h"
#include "compiler/library/bytes/bytes_lowerer/bytes_lowerer.h"
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
#include "compiler/program/ast/ast.h"
#include "compiler/program/types/types.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/Support/ErrorHandling.h"


namespace tz {

LazyChainLowerer::LazyChainLowerer(Lowering &owner)
    : lowering(owner) {}

llvm::Value *LazyChainLowerer::callCallback(llvm::Value *slot, const Type *fnType,
                                            std::vector<llvm::Value *> args,
                                            llvm::ArrayRef<const Type *> types) {
  size_t passes = std::min(fnType->params().size(), args.size());
  for (size_t i = passes; i < args.size(); ++i) {
    this->lowering.refCounts().releaseValue(args[i], types[i]);
  }
  args.resize(passes);
  llvm::Value *fn = this->lowering.builder_.CreateLoad(this->lowering.fnTy_, slot, "callback");
  llvm::Value *result = this->lowering.closureLowerer().callFunctionValue(fn, fnType, args);

  return result;
}

Lowering::Producer LazyChainLowerer::arrayProducer(llvm::Value *slot, const Type *arrayType,
                                                   Builtin mode) {
  Lowering::Producer producer;
  producer.run = [this, slot, arrayType, mode](const Lowering::Consumer &consumer) {
    const Type *element = arrayType->inner();
    llvm::Type *i64 = this->lowering.builder_.getInt64Ty();
    llvm::Value *indexSlot = this->lowering.functionFrame().createStackSlot(i64, "index");
    this->lowering.builder_.CreateStore(this->lowering.builder_.getInt64(0), indexSlot);
    llvm::BasicBlock *headBlock = this->lowering.functionFrame().newBlock("iter.array");
    llvm::BasicBlock *bodyBlock = this->lowering.functionFrame().newBlock("iter.array.body");
    llvm::BasicBlock *endBlock = this->lowering.functionFrame().newBlock("iter.array.end");
    this->lowering.functionFrame().emitBlock(headBlock);
    llvm::Value *array =
        this->lowering.builder_.CreateLoad(this->lowering.builder_.getPtrTy(), slot, "array");
    llvm::Value *index = this->lowering.builder_.CreateLoad(i64, indexSlot, "index");
    this->lowering.builder_.CreateCondBr(
        this->lowering.builder_.CreateICmpULT(index,
                                              this->lowering.arrayLowerer().arrayLength(array)),
        bodyBlock, endBlock);
    this->lowering.functionFrame().switchTo(bodyBlock);
    this->lowering.builder_.CreateStore(
        this->lowering.builder_.CreateAdd(index, this->lowering.builder_.getInt64(1)), indexSlot);
    llvm::Value *value;
    if (mode == Builtin::ArrKeys) {
      value = index;
    } else {
      value = this->lowering.builder_.CreateLoad(
          this->lowering.moduleBuilder().lower(element),
          this->lowering.arrayLowerer().elementAddress(
              this->lowering.arrayLowerer().arrayData(array), element, index),
          "element");
      this->lowering.refCounts().retainValue(value, element);
      if (mode == Builtin::ArrEntries) {
        const Type *tuple = Type::tuple({Type::intTy(), element});
        llvm::Value *entry = this->lowering.objectLayouts().allocObject(tuple, {0, 1});
        this->lowering.builder_.CreateStore(
            index, this->lowering.objectLayouts().fieldAddress(entry, tuple, "0"));
        this->lowering.builder_.CreateStore(
            value, this->lowering.objectLayouts().fieldAddress(entry, tuple, "1"));
        this->lowering.groupLowerer().adoptFields(entry, tuple);
        value = entry;
      }
    }
    llvm::Value *sent = consumer.each(value);
    if (sent) {
      this->lowering.refCounts().releaseValue(sent, consumer.sent);
    }
    this->lowering.functionFrame().branchTo(headBlock);
    this->lowering.functionFrame().switchTo(endBlock);
  };

  return producer;
}

Lowering::Producer LazyChainLowerer::valueProducer(llvm::Value *slot, const Type *type,
                                                   llvm::Value *returnSlot) {
  if (type->isArray()) {
    return this->arrayProducer(slot, type, Builtin::ArrValues);
  }
  if (type->isTypedArray()) {
    return this->lowering.bytesLowerer().bytesProducer(slot, type);
  }
  Lowering::Producer producer;
  if (type->isString()) {
    producer.run = [this, slot](const Lowering::Consumer &consumer) {
      llvm::Type *i64 = this->lowering.builder_.getInt64Ty();
      llvm::Value *indexSlot = this->lowering.functionFrame().createStackSlot(i64, "index");
      this->lowering.builder_.CreateStore(this->lowering.builder_.getInt64(0), indexSlot);
      llvm::BasicBlock *headBlock = this->lowering.functionFrame().newBlock("iter.string");
      llvm::BasicBlock *bodyBlock = this->lowering.functionFrame().newBlock("iter.string.body");
      llvm::BasicBlock *endBlock = this->lowering.functionFrame().newBlock("iter.string.end");
      this->lowering.functionFrame().emitBlock(headBlock);
      llvm::Value *str = this->lowering.builder_.CreateLoad(this->lowering.strTy_, slot, "string");
      llvm::Value *index = this->lowering.builder_.CreateLoad(i64, indexSlot, "index");
      this->lowering.builder_.CreateCondBr(
          this->lowering.builder_.CreateICmpULT(index,
                                                this->lowering.stringLowerer().stringLength(str)),
          bodyBlock, endBlock);
      this->lowering.functionFrame().switchTo(bodyBlock);
      llvm::Value *out =
          this->lowering.functionFrame().createStackSlot(this->lowering.strTy_, "code.point");
      llvm::Value *units = this->lowering.builder_.CreateCall(
          this->lowering.rt_.strCodePointAt,
          {out, this->lowering.stringLowerer().stringObj(str),
           this->lowering.stringLowerer().stringMeta(str), index});
      this->lowering.builder_.CreateStore(
          this->lowering.builder_.CreateAdd(index, this->lowering.builder_.CreateSExt(units, i64)),
          indexSlot);
      llvm::Value *sent =
          consumer.each(this->lowering.builder_.CreateLoad(this->lowering.strTy_, out));
      if (sent) {
        this->lowering.refCounts().releaseValue(sent, consumer.sent);
      }
      this->lowering.functionFrame().branchTo(headBlock);
      this->lowering.functionFrame().switchTo(endBlock);
    };
    return producer;
  }
  producer.run = [this, slot, type, returnSlot](const Lowering::Consumer &consumer) {
    const Type *element = type->yielded();
    llvm::Value *out = this->lowering.functionFrame().createStackSlot(
        this->lowering.moduleBuilder().lower(element), "next.value");
    llvm::Value *sentSlot = nullptr;
    llvm::Value *hasSent = nullptr;
    if (consumer.sent) {
      sentSlot = this->lowering.functionFrame().createStackSlot(
          this->lowering.moduleBuilder().lower(consumer.sent), "next.sent");
      hasSent = this->lowering.functionFrame().createStackSlot(this->lowering.builder_.getInt1Ty(),
                                                               "next.has.sent");
      this->lowering.builder_.CreateStore(this->lowering.builder_.getFalse(), hasSent);
    }
    llvm::BasicBlock *headBlock = this->lowering.functionFrame().newBlock("iter.next");
    llvm::BasicBlock *bodyBlock = this->lowering.functionFrame().newBlock("iter.next.body");
    llvm::BasicBlock *endBlock = this->lowering.functionFrame().newBlock("iter.next.end");
    this->lowering.functionFrame().emitBlock(headBlock);
    llvm::Value *it =
        this->lowering.builder_.CreateLoad(this->lowering.builder_.getPtrTy(), slot, "iterator");
    llvm::Value *in = nullptr;
    if (hasSent) {
      in = this->lowering.builder_.CreateSelect(
          this->lowering.builder_.CreateLoad(this->lowering.builder_.getInt1Ty(), hasSent),
          sentSlot, llvm::ConstantPointerNull::get(this->lowering.builder_.getPtrTy()));
    }
    llvm::Value *has = this->lowering.coroutineLowerer().emitResume(it, out, in, returnSlot);
    if (hasSent) {
      llvm::BasicBlock *releaseBlock = this->lowering.functionFrame().newBlock("next.release");
      llvm::BasicBlock *afterBlock = this->lowering.functionFrame().newBlock("next.after");
      this->lowering.builder_.CreateCondBr(
          this->lowering.builder_.CreateLoad(this->lowering.builder_.getInt1Ty(), hasSent),
          releaseBlock, afterBlock);
      this->lowering.functionFrame().switchTo(releaseBlock);
      this->lowering.refCounts().releaseValue(
          this->lowering.builder_.CreateLoad(this->lowering.moduleBuilder().lower(consumer.sent),
                                             sentSlot),
          consumer.sent);
      this->lowering.builder_.CreateStore(this->lowering.builder_.getFalse(), hasSent);
      this->lowering.builder_.CreateBr(afterBlock);
      this->lowering.functionFrame().switchTo(afterBlock);
    }
    this->lowering.builder_.CreateCondBr(has, bodyBlock, endBlock);
    this->lowering.functionFrame().switchTo(bodyBlock);
    llvm::Value *sent = consumer.each(this->lowering.builder_.CreateLoad(
        this->lowering.moduleBuilder().lower(element), out, "value"));
    if (sent && this->lowering.functionFrame().blockIsOpen()) {
      this->lowering.builder_.CreateStore(sent, sentSlot);
      this->lowering.builder_.CreateStore(this->lowering.builder_.getTrue(), hasSent);
    }
    this->lowering.functionFrame().branchTo(headBlock);
    this->lowering.functionFrame().switchTo(endBlock);
  };

  return producer;
}

Lowering::Consumer LazyChainLowerer::stageConsumer(Builtin stage, const Type *callbackType,
                                                   const Type *element, const Type *result,
                                                   llvm::Value *operand,
                                                   const Lowering::Consumer &down) {
  llvm::Type *i64 = this->lowering.builder_.getInt64Ty();
  Lowering::Consumer stageOut;
  stageOut.stop = down.stop;
  auto give = [this, down](llvm::Value *value) {
    llvm::Value *sent = down.each(value);
    if (sent) {
      this->lowering.refCounts().releaseValue(sent, down.sent);
    }
  };
  llvm::Value *counter = nullptr;
  if (callbackType) {
    counter = this->lowering.functionFrame().createStackSlot(this->lowering.builder_.getInt64Ty(),
                                                             "counter");
    this->lowering.builder_.CreateStore(this->lowering.builder_.getInt64(0), counter);
  }
  auto nextCount = [this, counter] {
    llvm::Value *count =
        this->lowering.builder_.CreateLoad(this->lowering.builder_.getInt64Ty(), counter, "count");
    this->lowering.builder_.CreateStore(
        this->lowering.builder_.CreateAdd(count, this->lowering.builder_.getInt64(1)), counter);

    return count;
  };

  switch (stage) {
  case Builtin::IterMap:
    stageOut.each = [=, this](llvm::Value *value) -> llvm::Value * {
      give(this->callCallback(operand, callbackType, {value, nextCount()},
                              {element, Type::intTy()}));

      return nullptr;
    };
    break;
  case Builtin::IterFilter:
    stageOut.each = [=, this](llvm::Value *value) -> llvm::Value * {
      this->lowering.refCounts().retainValue(value, element);
      llvm::Value *keep =
          this->callCallback(operand, callbackType, {value, nextCount()}, {element, Type::intTy()});
      llvm::BasicBlock *keepBlock = this->lowering.functionFrame().newBlock("filter.keep");
      llvm::BasicBlock *dropBlock = this->lowering.functionFrame().newBlock("filter.drop");
      llvm::BasicBlock *nextBlock = this->lowering.functionFrame().newBlock("filter.next");
      this->lowering.builder_.CreateCondBr(keep, keepBlock, dropBlock);
      this->lowering.functionFrame().switchTo(keepBlock);
      give(this->lowering.exprLowerer().emitConversion(value, element, result));
      this->lowering.functionFrame().branchTo(nextBlock);
      this->lowering.functionFrame().switchTo(dropBlock);
      this->lowering.refCounts().releaseValue(value, element);
      this->lowering.builder_.CreateBr(nextBlock);
      this->lowering.functionFrame().switchTo(nextBlock);

      return nullptr;
    };
    break;
  case Builtin::IterTake:
  case Builtin::IterDrop: {
    llvm::Value *left = this->lowering.functionFrame().createStackSlot(i64, "left");
    llvm::Value *count = this->lowering.builder_.CreateLoad(i64, operand, "limit");
    this->lowering.builder_.CreateStore(count, left);
    if (stage == Builtin::IterTake) {
      llvm::BasicBlock *zeroBlock = this->lowering.functionFrame().newBlock("take.none");
      llvm::BasicBlock *startBlock = this->lowering.functionFrame().newBlock("take.start");
      this->lowering.builder_.CreateCondBr(
          this->lowering.builder_.CreateICmpEQ(count, this->lowering.builder_.getInt64(0)),
          zeroBlock, startBlock);
      this->lowering.functionFrame().switchTo(zeroBlock);
      down.stop();
      this->lowering.functionFrame().switchTo(startBlock);
      stageOut.each = [=, this](llvm::Value *value) -> llvm::Value * {
        llvm::Value *rest = this->lowering.builder_.CreateSub(
            this->lowering.builder_.CreateLoad(i64, left), this->lowering.builder_.getInt64(1));
        this->lowering.builder_.CreateStore(rest, left);
        give(value);
        if (!this->lowering.functionFrame().blockIsOpen()) {
          return nullptr;
        }
        llvm::BasicBlock *doneBlock = this->lowering.functionFrame().newBlock("take.done");
        llvm::BasicBlock *moreBlock = this->lowering.functionFrame().newBlock("take.more");
        this->lowering.builder_.CreateCondBr(
            this->lowering.builder_.CreateICmpEQ(rest, this->lowering.builder_.getInt64(0)),
            doneBlock, moreBlock);
        this->lowering.functionFrame().switchTo(doneBlock);
        down.stop();
        this->lowering.functionFrame().switchTo(moreBlock);

        return nullptr;
      };
    } else {
      stageOut.each = [=, this](llvm::Value *value) -> llvm::Value * {
        llvm::Value *rest = this->lowering.builder_.CreateLoad(i64, left);
        llvm::BasicBlock *skipBlock = this->lowering.functionFrame().newBlock("drop.skip");
        llvm::BasicBlock *passBlock = this->lowering.functionFrame().newBlock("drop.pass");
        llvm::BasicBlock *nextBlock = this->lowering.functionFrame().newBlock("drop.next");
        this->lowering.builder_.CreateCondBr(
            this->lowering.builder_.CreateICmpSGT(rest, this->lowering.builder_.getInt64(0)),
            skipBlock, passBlock);
        this->lowering.functionFrame().switchTo(skipBlock);
        this->lowering.builder_.CreateStore(
            this->lowering.builder_.CreateSub(rest, this->lowering.builder_.getInt64(1)), left);
        this->lowering.refCounts().releaseValue(value, element);
        this->lowering.builder_.CreateBr(nextBlock);
        this->lowering.functionFrame().switchTo(passBlock);
        give(value);
        this->lowering.functionFrame().branchTo(nextBlock);
        this->lowering.functionFrame().switchTo(nextBlock);

        return nullptr;
      };
    }
    break;
  }
  case Builtin::IterFlatMap:
    stageOut.each = [=, this](llvm::Value *value) -> llvm::Value * {
      const Type *inner = callbackType->ret();
      llvm::Value *iterable =
          this->callCallback(operand, callbackType, {value, nextCount()}, {element, Type::intTy()});
      this->lowering.functionFrame().pushScope();
      llvm::Value *slot = this->lowering.functionFrame().createManagedSlot(inner, "flat");
      this->lowering.builder_.CreateStore(iterable, slot);
      Lowering::Consumer flat;
      flat.each = [=](llvm::Value *v) -> llvm::Value * {
        give(v);

        return nullptr;
      };
      flat.stop = down.stop;
      this->valueProducer(slot, inner).run(flat);
      this->lowering.functionFrame().popScope();

      return nullptr;
    };
    break;
  default:
    llvm_unreachable("not a stage of a chain");
  }

  return stageOut;
}

}
