#include "compiler/lower/iterators/iterator_lowerer/iterator_lowerer.h"
#include "compiler/lower/module_builder/module_builder.h"
#include "compiler/lower/function_frame/function_frame.h"
#include "compiler/lower/memory/ref_counts/ref_counts.h"
#include "compiler/lower/types/optional_lowerer/optional_lowerer.h"
#include "compiler/lower/stmt/stmt_lowerer/stmt_lowerer.h"
#include "compiler/lower/expr/expr_lowerer/expr_lowerer.h"
#include "compiler/lower/values/array_lowerer/array_lowerer.h"
#include "compiler/lower/memory/group_lowerer/group_lowerer.h"
#include "compiler/lower/memory/store_lowerer/store_lowerer.h"
#include "compiler/lower/iterators/coroutine_lowerer/coroutine_lowerer.h"
#include "compiler/lower/iterators/generator_lowerer/generator_lowerer.h"
#include "compiler/lower/iterators/lazy_chain_lowerer/lazy_chain_lowerer.h"
#include "compiler/lower/iterators/inline_generator_lowerer/inline_generator_lowerer.h"
#include "compiler/lower/layouts/object_layouts/object_layouts.h"
#include "compiler/lower/errors/throw_lowerer/throw_lowerer.h"
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

namespace {

// The header before the frame of the coroutine: { i64 rc, ptr layout }.
constexpr uint64_t kHeaderSize = 16;

// What iterating a value of `type` gives (see Sema::iteratedType).
const Type *iteratedType(const Type *type) {
  if (type->isTypedArray()) {
    return type->elementType();
  }

  return type->isArray() ? type->inner() : type->isString() ? type : type->yielded();
}

} // namespace

IteratorLowerer::IteratorLowerer(CodeGen &codegen) : codegen_(codegen) {}

void IteratorLowerer::emitIteratorForOf(ForOfStmt *stmt) {
  llvm::SmallVector<llvm::StringRef, 1> labels = std::move(codegen_.pendingLabels_);
  codegen_.pendingLabels_.clear();
  codegen_.functionFrame().pushScope(); // the operands of the iteration
  CodeGen::Producer producer = codegen_.inlineGeneratorLowerer().prepare(stmt->iterable);
  size_t depth = codegen_.scopes_.size();
  llvm::BasicBlock *endBlock = codegen_.functionFrame().newBlock("for.of.end");
  CodeGen::Consumer consumer;
  consumer.stop = [this, depth, endBlock] {
    codegen_.functionFrame().emitCleanups(depth);
    codegen_.builder_.CreateBr(endBlock);
  };
  consumer.each = [this, stmt, labels, depth, endBlock](llvm::Value *value) -> llvm::Value * {
    // `continue` goes on after the value, in a generator emitted inline
    // inside its scopes; `break` leaves them all.
    llvm::BasicBlock *nextBlock = codegen_.functionFrame().newBlock("for.of.next");
    size_t here = codegen_.scopes_.size();
    codegen_.functionFrame().pushScope(); // the loop variable, fresh in every iteration
    codegen_.storeLowerer().storeVar(stmt->binding.sym, codegen_.functionFrame().createLocal(stmt->binding.sym, stmt->binding.name), value,
             /*replacing=*/true);
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

void IteratorLowerer::emitIteratorSpread(llvm::Value *array, Expr *source, const Type *element) {
  codegen_.functionFrame().pushScope();
  CodeGen::Producer producer = codegen_.inlineGeneratorLowerer().prepare(source);
  size_t depth = codegen_.scopes_.size();
  llvm::BasicBlock *endBlock = codegen_.functionFrame().newBlock("spread.end");
  const Type *yielded = iteratedType(source->type);
  CodeGen::Consumer consumer;
  consumer.each = [this, array, yielded, element](llvm::Value *value) -> llvm::Value * {
    codegen_.arrayLowerer().emitPush(array, {codegen_.exprLowerer().emitConversion(value, yielded, element)}, element);

    return nullptr;
  };
  consumer.stop = [this, depth, endBlock] {
    codegen_.functionFrame().emitCleanups(depth);
    codegen_.builder_.CreateBr(endBlock);
  };
  producer.run(consumer);
  codegen_.functionFrame().emitBlock(endBlock);
  codegen_.functionFrame().popScope();
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
  codegen_.functionFrame().pushScope(); // the operands
  CodeGen::Producer producer = codegen_.inlineGeneratorLowerer().prepare(source);
  // The callback is evaluated after the chain, as JavaScript does.
  Expr *callbackExpr = from ? (call->args.size() > 1 ? call->args[1] : nullptr)
                       : method == Builtin::IterToArray ? nullptr
                                                        : call->args[0];
  const Type *callbackType = callbackExpr ? callbackExpr->type : nullptr;
  llvm::Value *callback = nullptr;
  if (callbackExpr) {
    callback = codegen_.functionFrame().createManagedSlot(callbackType, "callback");
    codegen_.builder_.CreateStore(codegen_.exprLowerer().emitExpr(callbackExpr), callback);
  }
  llvm::Type *i32 = codegen_.builder_.getInt32Ty();
  llvm::Value *counter = codegen_.functionFrame().createStackSlot(i32, "counter");
  codegen_.builder_.CreateStore(codegen_.builder_.getInt32(0), counter);
  auto nextCount = [this, counter, i32] {
    llvm::Value *count = codegen_.builder_.CreateLoad(i32, counter, "count");
    codegen_.builder_.CreateStore(codegen_.builder_.CreateAdd(count, codegen_.builder_.getInt32(1)), counter);

    return count;
  };
  size_t depth = codegen_.scopes_.size();
  llvm::BasicBlock *endBlock = codegen_.functionFrame().newBlock("iter.end");
  CodeGen::Consumer consumer;
  consumer.stop = [this, depth, endBlock] {
    codegen_.functionFrame().emitCleanups(depth);
    codegen_.builder_.CreateBr(endBlock);
  };

  llvm::Value *array = nullptr, *resultSlot = nullptr, *hasAcc = nullptr;
  const Type *resultType = call->type;
  // The array being filled waits in a slot, which an error releases.
  CodeGen::Held heldArray{nullptr, nullptr, nullptr};
  switch (method) {
  case Builtin::ArrayFrom:
  case Builtin::IterToArray: {
    const Type *stored = resultType->inner();
    array = codegen_.groupLowerer().newArray(Type::array(stored), codegen_.builder_.getInt64(0), codegen_.builder_.getInt64(0));
    heldArray = codegen_.throwLowerer().hold(array, resultType, true);
    consumer.each = [=, this](llvm::Value *value) -> llvm::Value * {
      if (callback) {
        value =
            codegen_.lazyChainLowerer().callCallback(callback, callbackType, {value, nextCount()}, {element, Type::intTy()});
      } else {
        value = codegen_.exprLowerer().emitConversion(value, element, stored);
      }
      codegen_.arrayLowerer().emitPush(array, {value}, stored);

      return nullptr;
    };
    break;
  }
  case Builtin::IterForEach:
    consumer.each = [=, this](llvm::Value *value) -> llvm::Value * {
      if (llvm::Value *result = codegen_.lazyChainLowerer().callCallback(callback, callbackType, {value, nextCount()},
                                             {element, Type::intTy()})) {
        codegen_.refCounts().releaseValue(result, callbackType->ret());
      }

      return nullptr;
    };
    break;
  case Builtin::IterReduce: {
    resultSlot = codegen_.functionFrame().createStackSlot(codegen_.moduleBuilder().lower(resultType), "acc");
    if (call->args.size() > 1) {
      codegen_.builder_.CreateStore(codegen_.exprLowerer().emitExpr(call->args[1]), resultSlot);
    } else {
      // Without an initial value the first value is the accumulator.
      hasAcc = codegen_.functionFrame().createStackSlot(codegen_.builder_.getInt1Ty(), "has.acc");
      codegen_.builder_.CreateStore(codegen_.builder_.getFalse(), hasAcc);
    }
    consumer.each = [=, this](llvm::Value *value) -> llvm::Value * {
      llvm::BasicBlock *firstBlock = nullptr, *nextBlock = nullptr;
      if (hasAcc) {
        firstBlock = codegen_.functionFrame().newBlock("reduce.first");
        nextBlock = codegen_.functionFrame().newBlock("reduce.next");
        llvm::BasicBlock *combineBlock = codegen_.functionFrame().newBlock("reduce.combine");
        codegen_.builder_.CreateCondBr(codegen_.builder_.CreateLoad(codegen_.builder_.getInt1Ty(), hasAcc), combineBlock,
                              firstBlock);
        codegen_.functionFrame().switchTo(firstBlock);
        codegen_.builder_.CreateStore(value, resultSlot);
        codegen_.builder_.CreateStore(codegen_.builder_.getTrue(), hasAcc);
        nextCount();
        codegen_.builder_.CreateBr(nextBlock);
        codegen_.functionFrame().switchTo(combineBlock);
      }
      llvm::Value *acc = codegen_.builder_.CreateLoad(codegen_.moduleBuilder().lower(resultType), resultSlot); // moves into the call
      codegen_.builder_.CreateStore(codegen_.lazyChainLowerer().callCallback(callback, callbackType, {acc, value, nextCount()},
                                        {resultType, element, Type::intTy()}),
                           resultSlot);
      if (nextBlock) {
        codegen_.functionFrame().emitBlock(nextBlock);
      }

      return nullptr;
    };
    break;
  }
  case Builtin::IterSome:
  case Builtin::IterEvery:
  case Builtin::IterFind: {
    resultSlot = codegen_.functionFrame().createStackSlot(codegen_.moduleBuilder().lower(resultType), "result");
    codegen_.builder_.CreateStore(method == Builtin::IterSome    ? codegen_.builder_.getFalse()
                         : method == Builtin::IterEvery ? codegen_.builder_.getTrue()
                                                        : codegen_.optionalLowerer().undefinedValue(resultType),
                         resultSlot);
    consumer.each = [=, this](llvm::Value *value) -> llvm::Value * {
      bool find = method == Builtin::IterFind;
      if (find) {
        codegen_.refCounts().retainValue(value, element); // kept if it is the one
      }
      llvm::Value *decides =
          codegen_.lazyChainLowerer().callCallback(callback, callbackType, {value, nextCount()}, {element, Type::intTy()});
      if (method == Builtin::IterEvery) {
        decides = codegen_.builder_.CreateNot(decides);
      }
      llvm::BasicBlock *foundBlock = codegen_.functionFrame().newBlock("iter.found");
      llvm::BasicBlock *otherBlock = codegen_.functionFrame().newBlock("iter.other");
      codegen_.builder_.CreateCondBr(decides, foundBlock, otherBlock);
      codegen_.functionFrame().switchTo(foundBlock);
      codegen_.builder_.CreateStore(find ? codegen_.optionalLowerer().makeOptional(value, codegen_.builder_.getTrue(), resultType)
                                : codegen_.builder_.getInt1(method == Builtin::IterSome),
                           resultSlot);
      consumer.stop();
      codegen_.functionFrame().switchTo(otherBlock);
      if (find) {
        codegen_.refCounts().releaseValue(value, element);
      }

      return nullptr;
    };
    break;
  }
  default:
    llvm_unreachable("not a consumer of an iterator");
  }
  producer.run(consumer);
  codegen_.functionFrame().emitBlock(endBlock);
  llvm::Value *result = nullptr;
  if (array) {
    result = codegen_.throwLowerer().take(heldArray);
  } else if (resultSlot) {
    if (hasAcc) {
      // As JavaScript, which reports a TypeError.
      llvm::BasicBlock *emptyBlock = codegen_.functionFrame().newBlock("reduce.empty");
      llvm::BasicBlock *okBlock = codegen_.functionFrame().newBlock("reduce.ok");
      codegen_.builder_.CreateCondBr(codegen_.builder_.CreateLoad(codegen_.builder_.getInt1Ty(), hasAcc), okBlock, emptyBlock);
      codegen_.functionFrame().switchTo(emptyBlock);
      codegen_.throwLowerer().emitThrowNew(1, "Reduce of a done iterator with no initial value");
      codegen_.functionFrame().switchTo(okBlock);
    }
    result = codegen_.builder_.CreateLoad(codegen_.moduleBuilder().lower(resultType), resultSlot);
  }
  codegen_.functionFrame().popScope();

  return result;
}

// `it.next(x)`: a new { value, done }.
llvm::Value *IteratorLowerer::emitNext(CallExpr *call) {
  auto *member = cast<MemberExpr>(call->callee);
  const Type *type = member->object->type;
  const Type *element = type->yielded(), *returned = type->returned(), *sent = type->sent();
  // Owned: the generator may drop every other reference to itself while it
  // runs. It waits in a slot, which an error from it releases.
  llvm::Value *it = codegen_.exprLowerer().emitExpr(member->object);
  CodeGen::Held heldIt = codegen_.throwLowerer().hold(it, type, true);
  llvm::Value *sentValue = nullptr, *in = nullptr;
  if (!call->args.empty()) {
    sentValue = codegen_.exprLowerer().emitExpr(call->args[0]);
    if (sent->isVoid()) {
      codegen_.refCounts().releaseValue(sentValue, call->args[0]->type); // the generator ignores it
      sentValue = nullptr;
    } else {
      in = codegen_.functionFrame().spill(sentValue, "sent");
    }
  }
  // Zero unless it gives a value: an undefined value holds zero.
  llvm::Value *out = codegen_.functionFrame().createStackSlot(codegen_.moduleBuilder().lower(element), "next.value");
  codegen_.builder_.CreateStore(llvm::Constant::getNullValue(codegen_.moduleBuilder().lower(element)), out);
  llvm::Value *ret = nullptr;
  if (!returned->isVoid()) {
    ret = codegen_.functionFrame().createStackSlot(codegen_.moduleBuilder().lower(returned), "next.return");
    codegen_.builder_.CreateStore(llvm::Constant::getNullValue(codegen_.moduleBuilder().lower(returned)), ret);
  }
  llvm::Value *frame =
      codegen_.builder_.CreateConstInBoundsGEP1_64(codegen_.builder_.getInt8Ty(), it, kHeaderSize, "frame");
  llvm::Value *wasDone =
      codegen_.builder_.CreateCall(codegen_.coroutineLowerer().coroIntrinsic(llvm::Intrinsic::coro_done), {frame}, "was.done");
  llvm::Value *has = codegen_.coroutineLowerer().emitResume(it, out, in, ret);
  if (sentValue) {
    codegen_.refCounts().releaseValue(sentValue, sent); // the generator took a copy of it
  }
  // `T | R | undefined`: what it gave, or what it returned.
  const Type *valueType = call->type->field("value")->type;
  llvm::Value *given = codegen_.exprLowerer().emitConversion(codegen_.builder_.CreateLoad(codegen_.moduleBuilder().lower(element), out), element, valueType);
  llvm::Value *value = codegen_.builder_.CreateSelect(has, given, codegen_.optionalLowerer().undefinedValue(valueType));
  if (ret) {
    // What a generator returns comes with the call that finished it.
    llvm::Value *returnedValue =
        codegen_.exprLowerer().emitConversion(codegen_.builder_.CreateLoad(codegen_.moduleBuilder().lower(returned), ret), returned, valueType);
    returnedValue = codegen_.builder_.CreateSelect(wasDone, codegen_.optionalLowerer().undefinedValue(valueType), returnedValue);
    value = codegen_.builder_.CreateSelect(has, value, returnedValue);
  }
  const Type *resultType = call->type;
  llvm::Value *result = codegen_.objectLayouts().allocObject(resultType, {0, 1});
  codegen_.builder_.CreateStore(value, codegen_.objectLayouts().fieldAddress(result, resultType, "value"));
  codegen_.builder_.CreateStore(codegen_.builder_.CreateNot(has), codegen_.objectLayouts().fieldAddress(result, resultType, "done"));
  codegen_.groupLowerer().adoptFields(result, resultType);
  codegen_.refCounts().releaseValue(codegen_.throwLowerer().take(heldIt), type);

  return result;
}

// The iterator of an array or a string, or a helper, as a value.
llvm::Value *IteratorLowerer::emitIteratorObject(CallExpr *call) {
  const Type *result = call->type;
  switch (call->builtin) {
  case Builtin::IterFrom: {
    Expr *arg = call->args[0];
    if (arg->type->isIterator()) {
      return codegen_.exprLowerer().emitExpr(arg); // its own iterator
    }
    Builtin kind = arg->type->isString() ? Builtin::IterFrom : Builtin::ArrValues;
    return codegen_.builder_.CreateCall(codegen_.generatorLowerer().iteratorFunction(kind, arg->type, nullptr, result), {codegen_.exprLowerer().emitExpr(arg)});
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
    return codegen_.builder_.CreateCall(codegen_.generatorLowerer().iteratorFunction(call->builtin, array->type, nullptr, result),
                               {codegen_.exprLowerer().emitExpr(array)});
  }
  default: {
    // A stage: the iterator before it, and its callback or count.
    Expr *receiver = cast<MemberExpr>(call->callee)->object;
    Expr *arg = call->args[0];
    llvm::Function *fn = codegen_.generatorLowerer().iteratorFunction(call->builtin, receiver->type, arg->type, result);
    llvm::Value *source = codegen_.exprLowerer().emitExpr(receiver);
    return codegen_.builder_.CreateCall(fn, {source, codegen_.exprLowerer().emitExpr(arg)});
  }
  }
}

} // namespace tz
