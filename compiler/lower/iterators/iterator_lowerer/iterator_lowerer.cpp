#include "compiler/lower/iterators/iterator_lowerer/iterator_lowerer.h"
#include "compiler/check/checker/checker.h"
#include "compiler/lower/errors/throw_lowerer/throw_lowerer.h"
#include "compiler/lower/expr/expr_lowerer/expr_lowerer.h"
#include "compiler/lower/function_frame/function_frame.h"
#include "compiler/lower/iterators/coroutine_lowerer/coroutine_lowerer.h"
#include "compiler/lower/iterators/generator_lowerer/generator_lowerer.h"
#include "compiler/lower/iterators/inline_generator_lowerer/inline_generator_lowerer.h"
#include "compiler/lower/iterators/lazy_chain_lowerer/lazy_chain_lowerer.h"
#include "compiler/lower/layouts/object_layouts/object_layouts.h"
#include "compiler/lower/memory/group_lowerer/group_lowerer.h"
#include "compiler/lower/memory/ref_counts/ref_counts.h"
#include "compiler/lower/memory/store_lowerer/store_lowerer.h"
#include "compiler/lower/module_builder/module_builder.h"
#include "compiler/lower/stmt/stmt_lowerer/stmt_lowerer.h"
#include "compiler/lower/types/optional_lowerer/optional_lowerer.h"
#include "compiler/lower/values/array_lowerer/array_lowerer.h"
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

// The header before the frame of the coroutine: { i64 rc, ptr layout }.
constexpr uint64_t kHeaderSize = 16;

// What iterating a value of `type` gives (see Checker::iteratedType).
const Type *iteratedType(const Type *type) {
  if (type->isTypedArray()) {
    return type->elementType();
  }

  return type->isArray() ? type->inner() : type->isString() ? type : type->yielded();
}

} // namespace

IteratorLowerer::IteratorLowerer(Lowering &lowering) : lowering_(lowering) {}

void IteratorLowerer::emitIteratorForOf(ForOfStmt *stmt) {
  llvm::SmallVector<llvm::StringRef, 1> labels = std::move(lowering_.pendingLabels_);
  lowering_.pendingLabels_.clear();
  lowering_.functionFrame().pushScope(); // the operands of the iteration
  Lowering::Producer producer = lowering_.inlineGeneratorLowerer().prepare(stmt->iterable);
  size_t depth = lowering_.scopes_.size();
  llvm::BasicBlock *endBlock = lowering_.functionFrame().newBlock("for.of.end");
  Lowering::Consumer consumer;
  consumer.stop = [this, depth, endBlock] {
    lowering_.functionFrame().emitCleanups(depth);
    lowering_.builder_.CreateBr(endBlock);
  };
  consumer.each = [this, stmt, labels, depth, endBlock](llvm::Value *value) -> llvm::Value * {
    // `continue` goes on after the value, in a generator emitted inline
    // inside its scopes; `break` leaves them all.
    llvm::BasicBlock *nextBlock = lowering_.functionFrame().newBlock("for.of.next");
    size_t here = lowering_.scopes_.size();
    lowering_.functionFrame().pushScope(); // the loop variable, fresh in every iteration
    lowering_.storeLowerer().storeVar(
        stmt->binding.sym,
        lowering_.functionFrame().createLocal(stmt->binding.sym, stmt->binding.name), value,
        /*replacing=*/true);
    lowering_.targets_.push_back(
        {labels, /*isLoop=*/true, /*isSwitch=*/false, endBlock, nextBlock, depth, here});
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

void IteratorLowerer::emitIteratorSpread(llvm::Value *array, Expr *source, const Type *element) {
  lowering_.functionFrame().pushScope();
  Lowering::Producer producer = lowering_.inlineGeneratorLowerer().prepare(source);
  size_t depth = lowering_.scopes_.size();
  llvm::BasicBlock *endBlock = lowering_.functionFrame().newBlock("spread.end");
  const Type *yielded = iteratedType(source->type);
  Lowering::Consumer consumer;
  consumer.each = [this, array, yielded, element](llvm::Value *value) -> llvm::Value * {
    lowering_.arrayLowerer().emitPush(
        array, {lowering_.exprLowerer().emitConversion(value, yielded, element)}, element);

    return nullptr;
  };
  consumer.stop = [this, depth, endBlock] {
    lowering_.functionFrame().emitCleanups(depth);
    lowering_.builder_.CreateBr(endBlock);
  };
  producer.run(consumer);
  lowering_.functionFrame().emitBlock(endBlock);
  lowering_.functionFrame().popScope();
}

llvm::Value *IteratorLowerer::emitIteratorBuiltin(CallExpr *call) {
  switch (call->builtin) {
  case Builtin::IterNext:
    return emitNext(call);
  case Builtin::ArrayFrom:
  case Builtin::IterToArray:
  case Builtin::IterForEach:
  case Builtin::IterReduce:
  case Builtin::IterSome:
  case Builtin::IterEvery:
  case Builtin::IterFind:
    return emitIteratorSink(call);
  default:
    return emitIteratorObject(call);
  }
}

// toArray, forEach, reduce, some, every, find and Array.from: the chain
// before them runs here as one loop.
llvm::Value *IteratorLowerer::emitIteratorSink(CallExpr *call) {
  Builtin method = call->builtin;
  bool from = method == Builtin::ArrayFrom;
  Expr *source = from ? call->args[0] : cast<MemberExpr>(call->callee)->object;
  const Type *element = iteratedType(source->type);
  lowering_.functionFrame().pushScope(); // the operands
  Lowering::Producer producer = lowering_.inlineGeneratorLowerer().prepare(source);
  // The callback is evaluated after the chain, as JavaScript does.
  Expr *callbackExpr = from ? (call->args.size() > 1 ? call->args[1] : nullptr)
                       : method == Builtin::IterToArray ? nullptr
                                                        : call->args[0];
  const Type *callbackType = callbackExpr ? callbackExpr->type : nullptr;
  llvm::Value *callback = nullptr;
  if (callbackExpr) {
    callback = lowering_.functionFrame().createManagedSlot(callbackType, "callback");
    lowering_.builder_.CreateStore(lowering_.exprLowerer().emitExpr(callbackExpr), callback);
  }
  llvm::Type *i32 = lowering_.builder_.getInt32Ty();
  llvm::Value *counter = lowering_.functionFrame().createStackSlot(i32, "counter");
  lowering_.builder_.CreateStore(lowering_.builder_.getInt32(0), counter);
  auto nextCount = [this, counter, i32] {
    llvm::Value *count = lowering_.builder_.CreateLoad(i32, counter, "count");
    lowering_.builder_.CreateStore(
        lowering_.builder_.CreateAdd(count, lowering_.builder_.getInt32(1)), counter);

    return count;
  };
  size_t depth = lowering_.scopes_.size();
  llvm::BasicBlock *endBlock = lowering_.functionFrame().newBlock("iter.end");
  Lowering::Consumer consumer;
  consumer.stop = [this, depth, endBlock] {
    lowering_.functionFrame().emitCleanups(depth);
    lowering_.builder_.CreateBr(endBlock);
  };

  llvm::Value *array = nullptr, *resultSlot = nullptr, *hasAcc = nullptr;
  const Type *resultType = call->type;
  // The array being filled waits in a slot, which an error releases.
  Lowering::Held heldArray{nullptr, nullptr, nullptr};
  switch (method) {
  case Builtin::ArrayFrom:
  case Builtin::IterToArray: {
    const Type *stored = resultType->inner();
    array = lowering_.groupLowerer().newArray(Type::array(stored), lowering_.builder_.getInt64(0),
                                              lowering_.builder_.getInt64(0));
    heldArray = lowering_.throwLowerer().hold(array, resultType, true);
    consumer.each = [=, this](llvm::Value *value) -> llvm::Value * {
      if (callback) {
        value = lowering_.lazyChainLowerer().callCallback(
            callback, callbackType, {value, nextCount()}, {element, Type::intTy()});
      } else {
        value = lowering_.exprLowerer().emitConversion(value, element, stored);
      }
      lowering_.arrayLowerer().emitPush(array, {value}, stored);

      return nullptr;
    };
    break;
  }
  case Builtin::IterForEach:
    consumer.each = [=, this](llvm::Value *value) -> llvm::Value * {
      if (llvm::Value *result = lowering_.lazyChainLowerer().callCallback(
              callback, callbackType, {value, nextCount()}, {element, Type::intTy()})) {
        lowering_.refCounts().releaseValue(result, callbackType->ret());
      }

      return nullptr;
    };
    break;
  case Builtin::IterReduce: {
    resultSlot = lowering_.functionFrame().createStackSlot(
        lowering_.moduleBuilder().lower(resultType), "acc");
    if (call->args.size() > 1) {
      lowering_.builder_.CreateStore(lowering_.exprLowerer().emitExpr(call->args[1]), resultSlot);
    } else {
      // Without an initial value the first value is the accumulator.
      hasAcc = lowering_.functionFrame().createStackSlot(lowering_.builder_.getInt1Ty(), "has.acc");
      lowering_.builder_.CreateStore(lowering_.builder_.getFalse(), hasAcc);
    }
    consumer.each = [=, this](llvm::Value *value) -> llvm::Value * {
      llvm::BasicBlock *firstBlock = nullptr, *nextBlock = nullptr;
      if (hasAcc) {
        firstBlock = lowering_.functionFrame().newBlock("reduce.first");
        nextBlock = lowering_.functionFrame().newBlock("reduce.next");
        llvm::BasicBlock *combineBlock = lowering_.functionFrame().newBlock("reduce.combine");
        lowering_.builder_.CreateCondBr(
            lowering_.builder_.CreateLoad(lowering_.builder_.getInt1Ty(), hasAcc), combineBlock,
            firstBlock);
        lowering_.functionFrame().switchTo(firstBlock);
        lowering_.builder_.CreateStore(value, resultSlot);
        lowering_.builder_.CreateStore(lowering_.builder_.getTrue(), hasAcc);
        nextCount();
        lowering_.builder_.CreateBr(nextBlock);
        lowering_.functionFrame().switchTo(combineBlock);
      }
      llvm::Value *acc = lowering_.builder_.CreateLoad(lowering_.moduleBuilder().lower(resultType),
                                                       resultSlot); // moves into the call
      lowering_.builder_.CreateStore(lowering_.lazyChainLowerer().callCallback(
                                         callback, callbackType, {acc, value, nextCount()},
                                         {resultType, element, Type::intTy()}),
                                     resultSlot);
      if (nextBlock) {
        lowering_.functionFrame().emitBlock(nextBlock);
      }

      return nullptr;
    };
    break;
  }
  case Builtin::IterSome:
  case Builtin::IterEvery:
  case Builtin::IterFind: {
    resultSlot = lowering_.functionFrame().createStackSlot(
        lowering_.moduleBuilder().lower(resultType), "result");
    lowering_.builder_.CreateStore(method == Builtin::IterSome ? lowering_.builder_.getFalse()
                                   : method == Builtin::IterEvery
                                       ? lowering_.builder_.getTrue()
                                       : lowering_.optionalLowerer().undefinedValue(resultType),
                                   resultSlot);
    consumer.each = [=, this](llvm::Value *value) -> llvm::Value * {
      bool find = method == Builtin::IterFind;
      if (find) {
        lowering_.refCounts().retainValue(value, element); // kept if it is the one
      }
      llvm::Value *decides = lowering_.lazyChainLowerer().callCallback(
          callback, callbackType, {value, nextCount()}, {element, Type::intTy()});
      if (method == Builtin::IterEvery) {
        decides = lowering_.builder_.CreateNot(decides);
      }
      llvm::BasicBlock *foundBlock = lowering_.functionFrame().newBlock("iter.found");
      llvm::BasicBlock *otherBlock = lowering_.functionFrame().newBlock("iter.other");
      lowering_.builder_.CreateCondBr(decides, foundBlock, otherBlock);
      lowering_.functionFrame().switchTo(foundBlock);
      lowering_.builder_.CreateStore(find ? lowering_.optionalLowerer().makeOptional(
                                                value, lowering_.builder_.getTrue(), resultType)
                                          : lowering_.builder_.getInt1(method == Builtin::IterSome),
                                     resultSlot);
      consumer.stop();
      lowering_.functionFrame().switchTo(otherBlock);
      if (find) {
        lowering_.refCounts().releaseValue(value, element);
      }

      return nullptr;
    };
    break;
  }
  default:
    llvm_unreachable("not a consumer of an iterator");
  }
  producer.run(consumer);
  lowering_.functionFrame().emitBlock(endBlock);
  llvm::Value *result = nullptr;
  if (array) {
    result = lowering_.throwLowerer().take(heldArray);
  } else if (resultSlot) {
    if (hasAcc) {
      // As JavaScript, which reports a TypeError.
      llvm::BasicBlock *emptyBlock = lowering_.functionFrame().newBlock("reduce.empty");
      llvm::BasicBlock *okBlock = lowering_.functionFrame().newBlock("reduce.ok");
      lowering_.builder_.CreateCondBr(
          lowering_.builder_.CreateLoad(lowering_.builder_.getInt1Ty(), hasAcc), okBlock,
          emptyBlock);
      lowering_.functionFrame().switchTo(emptyBlock);
      lowering_.throwLowerer().emitThrowNew(1, "Reduce of a done iterator with no initial value");
      lowering_.functionFrame().switchTo(okBlock);
    }
    result = lowering_.builder_.CreateLoad(lowering_.moduleBuilder().lower(resultType), resultSlot);
  }
  lowering_.functionFrame().popScope();

  return result;
}

// `it.next(x)`: a new { value, done }.
llvm::Value *IteratorLowerer::emitNext(CallExpr *call) {
  auto *member = cast<MemberExpr>(call->callee);
  const Type *type = member->object->type;
  const Type *element = type->yielded(), *returned = type->returned(), *sent = type->sent();
  // Owned: the generator may drop every other reference to itself while it
  // runs. It waits in a slot, which an error from it releases.
  llvm::Value *it = lowering_.exprLowerer().emitExpr(member->object);
  Lowering::Held heldIt = lowering_.throwLowerer().hold(it, type, true);
  llvm::Value *sentValue = nullptr, *in = nullptr;
  if (!call->args.empty()) {
    sentValue = lowering_.exprLowerer().emitExpr(call->args[0]);
    if (sent->isVoid()) {
      lowering_.refCounts().releaseValue(sentValue,
                                         call->args[0]->type); // the generator ignores it
      sentValue = nullptr;
    } else {
      in = lowering_.functionFrame().spill(sentValue, "sent");
    }
  }
  // Zero unless it gives a value: an undefined value holds zero.
  llvm::Value *out = lowering_.functionFrame().createStackSlot(
      lowering_.moduleBuilder().lower(element), "next.value");
  lowering_.builder_.CreateStore(
      llvm::Constant::getNullValue(lowering_.moduleBuilder().lower(element)), out);
  llvm::Value *ret = nullptr;
  if (!returned->isVoid()) {
    ret = lowering_.functionFrame().createStackSlot(lowering_.moduleBuilder().lower(returned),
                                                    "next.return");
    lowering_.builder_.CreateStore(
        llvm::Constant::getNullValue(lowering_.moduleBuilder().lower(returned)), ret);
  }
  llvm::Value *frame = lowering_.builder_.CreateConstInBoundsGEP1_64(lowering_.builder_.getInt8Ty(),
                                                                     it, kHeaderSize, "frame");
  llvm::Value *wasDone = lowering_.builder_.CreateCall(
      lowering_.coroutineLowerer().coroIntrinsic(llvm::Intrinsic::coro_done), {frame}, "was.done");
  llvm::Value *has = lowering_.coroutineLowerer().emitResume(it, out, in, ret);
  if (sentValue) {
    lowering_.refCounts().releaseValue(sentValue, sent); // the generator took a copy of it
  }
  // `T | R | undefined`: what it gave, or what it returned.
  const Type *valueType = call->type->field("value")->type;
  llvm::Value *given = lowering_.exprLowerer().emitConversion(
      lowering_.builder_.CreateLoad(lowering_.moduleBuilder().lower(element), out), element,
      valueType);
  llvm::Value *value = lowering_.builder_.CreateSelect(
      has, given, lowering_.optionalLowerer().undefinedValue(valueType));
  if (ret) {
    // What a generator returns comes with the call that finished it.
    llvm::Value *returnedValue = lowering_.exprLowerer().emitConversion(
        lowering_.builder_.CreateLoad(lowering_.moduleBuilder().lower(returned), ret), returned,
        valueType);
    returnedValue = lowering_.builder_.CreateSelect(
        wasDone, lowering_.optionalLowerer().undefinedValue(valueType), returnedValue);
    value = lowering_.builder_.CreateSelect(has, value, returnedValue);
  }
  const Type *resultType = call->type;
  llvm::Value *result = lowering_.objectLayouts().allocObject(resultType, {0, 1});
  lowering_.builder_.CreateStore(
      value, lowering_.objectLayouts().fieldAddress(result, resultType, "value"));
  lowering_.builder_.CreateStore(
      lowering_.builder_.CreateNot(has),
      lowering_.objectLayouts().fieldAddress(result, resultType, "done"));
  lowering_.groupLowerer().adoptFields(result, resultType);
  lowering_.refCounts().releaseValue(lowering_.throwLowerer().take(heldIt), type);

  return result;
}

// The iterator of an array or a string, or a helper, as a value.
llvm::Value *IteratorLowerer::emitIteratorObject(CallExpr *call) {
  const Type *result = call->type;
  switch (call->builtin) {
  case Builtin::IterFrom: {
    Expr *arg = call->args[0];
    if (arg->type->isIterator()) {
      return lowering_.exprLowerer().emitExpr(arg); // its own iterator
    }
    Builtin kind = arg->type->isString() ? Builtin::IterFrom : Builtin::ArrValues;
    return lowering_.builder_.CreateCall(
        lowering_.generatorLowerer().iteratorFunction(kind, arg->type, nullptr, result),
        {lowering_.exprLowerer().emitExpr(arg)});
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
    return lowering_.builder_.CreateCall(
        lowering_.generatorLowerer().iteratorFunction(call->builtin, array->type, nullptr, result),
        {lowering_.exprLowerer().emitExpr(array)});
  }
  default: {
    // A stage: the iterator before it, and its callback or count.
    Expr *receiver = cast<MemberExpr>(call->callee)->object;
    Expr *arg = call->args[0];
    llvm::Function *fn = lowering_.generatorLowerer().iteratorFunction(
        call->builtin, receiver->type, arg->type, result);
    llvm::Value *source = lowering_.exprLowerer().emitExpr(receiver);
    return lowering_.builder_.CreateCall(fn, {source, lowering_.exprLowerer().emitExpr(arg)});
  }
  }
}

} // namespace tz
