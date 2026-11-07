#include "compiler/lower/iterators/iterator_lowerer/iterator_lowerer.h"
#include "compiler/check/checker/checker.h"
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

using llvm::cast;

namespace tz {

namespace {

constexpr uint64_t kHeaderSize = 16;

}

const Type *IteratorLowerer::iteratedType(const Type *type) {
  if (type->isTypedArray()) {
    return type->elementType();
  }

  return type->isArray() ? type->inner() : type->isString() ? type : type->yielded();
}

IteratorLowerer::IteratorLowerer(Lowering &owner)
    : lowering(owner) {}

void IteratorLowerer::emitIteratorForOf(ForOfStmt *stmt) {
  llvm::SmallVector<llvm::StringRef, 1> labels = std::move(this->lowering.pendingLabels_);
  this->lowering.pendingLabels_.clear();
  this->lowering.functionFrame().pushScope();
  Lowering::Producer producer = this->lowering.inlineGeneratorLowerer().prepare(stmt->iterable);
  size_t depth = this->lowering.scopes_.size();
  llvm::BasicBlock *endBlock = this->lowering.functionFrame().newBlock("for.of.end");
  Lowering::Consumer consumer;
  consumer.stop = [this, depth, endBlock] {
    this->lowering.functionFrame().emitCleanups(depth);
    this->lowering.builder_.CreateBr(endBlock);
  };
  consumer.each = [this, stmt, labels, depth, endBlock](llvm::Value *value) -> llvm::Value * {
    llvm::BasicBlock *nextBlock = this->lowering.functionFrame().newBlock("for.of.next");
    size_t here = this->lowering.scopes_.size();
    this->lowering.functionFrame().pushScope();
    this->lowering.storeLowerer().storeVar(
        stmt->binding.sym,
        this->lowering.functionFrame().createLocal(stmt->binding.sym, stmt->binding.name), value,
        true);
    this->lowering.targets_.push_back({labels, true, false, endBlock, nextBlock, depth, here});
    this->lowering.stmtLowerer().emitStmt(stmt->body);
    this->lowering.targets_.pop_back();
    this->lowering.functionFrame().popScope();
    this->lowering.functionFrame().emitBlock(nextBlock);

    return nullptr;
  };
  producer.run(consumer);
  this->lowering.functionFrame().emitBlock(endBlock);
  this->lowering.functionFrame().popScope();
}

void IteratorLowerer::emitIteratorSpread(llvm::Value *array, Expr *source, const Type *element) {
  this->lowering.functionFrame().pushScope();
  Lowering::Producer producer = this->lowering.inlineGeneratorLowerer().prepare(source);
  size_t depth = this->lowering.scopes_.size();
  llvm::BasicBlock *endBlock = this->lowering.functionFrame().newBlock("spread.end");
  const Type *yielded = IteratorLowerer::iteratedType(source->type);
  Lowering::Consumer consumer;
  consumer.each = [this, array, yielded, element](llvm::Value *value) -> llvm::Value * {
    this->lowering.arrayLowerer().emitPush(
        array, {this->lowering.exprLowerer().emitConversion(value, yielded, element)}, element);

    return nullptr;
  };
  consumer.stop = [this, depth, endBlock] {
    this->lowering.functionFrame().emitCleanups(depth);
    this->lowering.builder_.CreateBr(endBlock);
  };
  producer.run(consumer);
  this->lowering.functionFrame().emitBlock(endBlock);
  this->lowering.functionFrame().popScope();
}

llvm::Value *IteratorLowerer::emitIteratorBuiltin(CallExpr *call) {
  switch (call->builtin()) {
    case Builtin::IterNext:
      return this->emitNext(call);
    case Builtin::ArrayFrom:
    case Builtin::IterToArray:
    case Builtin::IterForEach:
    case Builtin::IterReduce:
    case Builtin::IterSome:
    case Builtin::IterEvery:
    case Builtin::IterFind:
      return this->emitIteratorSink(call);
    default:
      return this->emitIteratorObject(call);
  }
}

llvm::Value *IteratorLowerer::emitIteratorSink(CallExpr *call) {
  Builtin method = call->builtin();
  bool from = method == Builtin::ArrayFrom;
  Expr *source = from ? call->args[0] : cast<MemberExpr>(call->callee)->object;
  const Type *element = IteratorLowerer::iteratedType(source->type);
  this->lowering.functionFrame().pushScope();
  Lowering::Producer producer = this->lowering.inlineGeneratorLowerer().prepare(source);
  Expr *callbackExpr = from ? (call->args.size() > 1 ? call->args[1] : nullptr)
                       : method == Builtin::IterToArray ? nullptr
                                                        : call->args[0];
  const Type *callbackType = callbackExpr ? callbackExpr->type : nullptr;
  llvm::Value *callback = nullptr;
  if (callbackExpr) {
    callback = this->lowering.functionFrame().createManagedSlot(callbackType, "callback");
    this->lowering.builder_.CreateStore(this->lowering.exprLowerer().emitExpr(callbackExpr),
                                        callback);
  }
  llvm::Type *i64 = this->lowering.builder_.getInt64Ty();
  llvm::Value *counter = this->lowering.functionFrame().createStackSlot(i64, "counter");
  this->lowering.builder_.CreateStore(this->lowering.builder_.getInt64(0), counter);
  auto nextCount = [this, counter, i64] {
    llvm::Value *count = this->lowering.builder_.CreateLoad(i64, counter, "count");
    this->lowering.builder_.CreateStore(
        this->lowering.builder_.CreateAdd(count, this->lowering.builder_.getInt64(1)), counter);

    return count;
  };
  size_t depth = this->lowering.scopes_.size();
  llvm::BasicBlock *endBlock = this->lowering.functionFrame().newBlock("iter.end");
  Lowering::Consumer consumer;
  consumer.stop = [this, depth, endBlock] {
    this->lowering.functionFrame().emitCleanups(depth);
    this->lowering.builder_.CreateBr(endBlock);
  };

  llvm::Value *array = nullptr;
  llvm::Value *resultSlot = nullptr;
  llvm::Value *hasAcc = nullptr;
  const Type *resultType = call->type;
  const Type *accType = call->type;
  switch (method) {
  case Builtin::ArrayFrom:
  case Builtin::IterToArray: {
    const Type *stored = resultType->inner();
    array = this->lowering.groupLowerer().newArray(Type::array(stored),
                                                   this->lowering.builder_.getInt64(0),
                                                   this->lowering.builder_.getInt64(0));
    consumer.each = [=, this](llvm::Value *value) -> llvm::Value * {
      if (callback) {
        value = this->lowering.lazyChainLowerer().callCallback(
            callback, callbackType, {value, nextCount()}, {element, Type::intTy()});
      } else {
        value = this->lowering.exprLowerer().emitConversion(value, element, stored);
      }
      this->lowering.arrayLowerer().emitPush(array, {value}, stored);

      return nullptr;
    };
    break;
  }
  case Builtin::IterForEach:
    consumer.each = [=, this](llvm::Value *value) -> llvm::Value * {
      if (llvm::Value *result = this->lowering.lazyChainLowerer().callCallback(
              callback, callbackType, {value, nextCount()}, {element, Type::intTy()})) {
        this->lowering.refCounts().releaseValue(result, callbackType->ret());
      }

      return nullptr;
    };
    break;
  case Builtin::IterReduce: {
    accType = call->args.size() > 1 ? resultType : element;
    resultSlot = this->lowering.functionFrame().createStackSlot(
        this->lowering.moduleBuilder().lower(accType), "acc");
    if (call->args.size() > 1) {
      this->lowering.builder_.CreateStore(this->lowering.exprLowerer().emitExpr(call->args[1]),
                                          resultSlot);
    } else {
      this->lowering.builder_.CreateStore(
          llvm::Constant::getNullValue(this->lowering.moduleBuilder().lower(accType)), resultSlot);
      hasAcc = this->lowering.functionFrame().createStackSlot(this->lowering.builder_.getInt1Ty(),
                                                              "has.acc");
      this->lowering.builder_.CreateStore(this->lowering.builder_.getFalse(), hasAcc);
    }
    consumer.each = [=, this](llvm::Value *value) -> llvm::Value * {
      llvm::BasicBlock *firstBlock = nullptr;
      llvm::BasicBlock *nextBlock = nullptr;
      if (hasAcc) {
        firstBlock = this->lowering.functionFrame().newBlock("reduce.first");
        nextBlock = this->lowering.functionFrame().newBlock("reduce.next");
        llvm::BasicBlock *combineBlock = this->lowering.functionFrame().newBlock("reduce.combine");
        this->lowering.builder_.CreateCondBr(
            this->lowering.builder_.CreateLoad(this->lowering.builder_.getInt1Ty(), hasAcc),
            combineBlock, firstBlock);
        this->lowering.functionFrame().switchTo(firstBlock);
        this->lowering.builder_.CreateStore(value, resultSlot);
        this->lowering.builder_.CreateStore(this->lowering.builder_.getTrue(), hasAcc);
        nextCount();
        this->lowering.builder_.CreateBr(nextBlock);
        this->lowering.functionFrame().switchTo(combineBlock);
      }
      llvm::Value *acc = this->lowering.builder_.CreateLoad(
          this->lowering.moduleBuilder().lower(accType), resultSlot);
      this->lowering.builder_.CreateStore(
          this->lowering.lazyChainLowerer().callCallback(
              callback, callbackType, {acc, value, nextCount()}, {accType, element, Type::intTy()}),
          resultSlot);
      if (nextBlock) {
        this->lowering.functionFrame().emitBlock(nextBlock);
      }

      return nullptr;
    };
    break;
  }
  case Builtin::IterSome:
  case Builtin::IterEvery:
  case Builtin::IterFind: {
    resultSlot = this->lowering.functionFrame().createStackSlot(
        this->lowering.moduleBuilder().lower(resultType), "result");
    this->lowering.builder_.CreateStore(
        method == Builtin::IterSome ? this->lowering.builder_.getFalse()
        : method == Builtin::IterEvery
            ? this->lowering.builder_.getTrue()
            : this->lowering.optionalLowerer().undefinedValue(resultType),
        resultSlot);
    consumer.each = [=, this](llvm::Value *value) -> llvm::Value * {
      bool find = method == Builtin::IterFind;
      if (find) {
        this->lowering.refCounts().retainValue(value, element);
      }
      llvm::Value *decides = this->lowering.lazyChainLowerer().callCallback(
          callback, callbackType, {value, nextCount()}, {element, Type::intTy()});
      if (method == Builtin::IterEvery) {
        decides = this->lowering.builder_.CreateNot(decides);
      }
      llvm::BasicBlock *foundBlock = this->lowering.functionFrame().newBlock("iter.found");
      llvm::BasicBlock *otherBlock = this->lowering.functionFrame().newBlock("iter.other");
      this->lowering.builder_.CreateCondBr(decides, foundBlock, otherBlock);
      this->lowering.functionFrame().switchTo(foundBlock);
      this->lowering.builder_.CreateStore(
          find ? this->lowering.optionalLowerer().makeOptional(
                     value, this->lowering.builder_.getTrue(), resultType)
               : this->lowering.builder_.getInt1(method == Builtin::IterSome),
          resultSlot);
      consumer.stop();
      this->lowering.functionFrame().switchTo(otherBlock);
      if (find) {
        this->lowering.refCounts().releaseValue(value, element);
      }

      return nullptr;
    };
    break;
  }
  default:
    llvm_unreachable("not a consumer of an iterator");
  }
  producer.run(consumer);
  this->lowering.functionFrame().emitBlock(endBlock);
  llvm::Value *result = nullptr;
  if (array) {
    result = array;
  } else if (resultSlot) {
    result = this->lowering.builder_.CreateLoad(this->lowering.moduleBuilder().lower(accType),
                                                resultSlot);
    if (hasAcc) {
      llvm::Value *has =
          this->lowering.builder_.CreateLoad(this->lowering.builder_.getInt1Ty(), hasAcc);
      result = accType == resultType
                   ? this->lowering.builder_.CreateSelect(
                         has, result, this->lowering.optionalLowerer().undefinedValue(resultType))
                   : this->lowering.optionalLowerer().makeOptional(result, has, resultType);
    }
  }
  this->lowering.functionFrame().popScope();

  return result;
}

llvm::Value *IteratorLowerer::emitNext(CallExpr *call) {
  auto *member = cast<MemberExpr>(call->callee);
  const Type *type = member->object->type;
  const Type *element = type->yielded();
  const Type *returned = type->returned();
  const Type *sent = type->sent();
  llvm::Value *it = this->lowering.exprLowerer().emitExpr(member->object);
  llvm::Value *sentValue = nullptr;
  llvm::Value *in = nullptr;
  if (!call->args.empty()) {
    sentValue = this->lowering.exprLowerer().emitExpr(call->args[0]);
    if (sent->isVoid()) {
      this->lowering.refCounts().releaseValue(sentValue, call->args[0]->type);
      sentValue = nullptr;
    } else {
      in = this->lowering.functionFrame().spill(sentValue, "sent");
    }
  }
  llvm::Value *out = this->lowering.functionFrame().createStackSlot(
      this->lowering.moduleBuilder().lower(element), "next.value");
  this->lowering.builder_.CreateStore(
      llvm::Constant::getNullValue(this->lowering.moduleBuilder().lower(element)), out);
  llvm::Value *ret = nullptr;
  if (!returned->isVoid()) {
    ret = this->lowering.functionFrame().createStackSlot(
        this->lowering.moduleBuilder().lower(returned), "next.return");
    this->lowering.builder_.CreateStore(
        llvm::Constant::getNullValue(this->lowering.moduleBuilder().lower(returned)), ret);
  }
  llvm::Value *frame = this->lowering.builder_.CreateConstInBoundsGEP1_64(
      this->lowering.builder_.getInt8Ty(), it, kHeaderSize, "frame");
  llvm::Value *wasDone = this->lowering.builder_.CreateCall(
      this->lowering.coroutineLowerer().coroIntrinsic(llvm::Intrinsic::coro_done), {frame},
      "was.done");
  llvm::Value *has = this->lowering.coroutineLowerer().emitResume(it, out, in, ret);
  if (sentValue) {
    this->lowering.refCounts().releaseValue(sentValue, sent);
  }
  const Type *valueType = call->type->field("value")->type;
  llvm::Value *given = this->lowering.exprLowerer().emitConversion(
      this->lowering.builder_.CreateLoad(this->lowering.moduleBuilder().lower(element), out),
      element, valueType);
  llvm::Value *value = this->lowering.builder_.CreateSelect(
      has, given, this->lowering.optionalLowerer().undefinedValue(valueType));
  if (ret) {
    llvm::Value *returnedValue = this->lowering.exprLowerer().emitConversion(
        this->lowering.builder_.CreateLoad(this->lowering.moduleBuilder().lower(returned), ret),
        returned, valueType);
    returnedValue = this->lowering.builder_.CreateSelect(
        wasDone, this->lowering.optionalLowerer().undefinedValue(valueType), returnedValue);
    value = this->lowering.builder_.CreateSelect(has, value, returnedValue);
  }
  const Type *resultType = call->type;
  llvm::Value *result = this->lowering.objectLayouts().allocObject(resultType, {0, 1});
  this->lowering.builder_.CreateStore(
      value, this->lowering.objectLayouts().fieldAddress(result, resultType, "value"));
  this->lowering.builder_.CreateStore(
      this->lowering.builder_.CreateNot(has),
      this->lowering.objectLayouts().fieldAddress(result, resultType, "done"));
  this->lowering.groupLowerer().adoptFields(result, resultType);
  this->lowering.refCounts().releaseValue(it, type);

  return result;
}

llvm::Value *IteratorLowerer::emitIteratorObject(CallExpr *call) {
  const Type *result = call->type;
  switch (call->builtin()) {
    case Builtin::IterFrom: {
      Expr *arg = call->args[0];
      if (arg->type->isIterator()) {
        return this->lowering.exprLowerer().emitExpr(arg);
      }
      Builtin kind = arg->type->isString() ? Builtin::IterFrom : Builtin::ArrValues;
      return this->lowering.builder_.CreateCall(
          this->lowering.generatorLowerer().iteratorFunction(kind, arg->type, nullptr, result),
          {this->lowering.exprLowerer().emitExpr(arg)});
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
      return this->lowering.builder_.CreateCall(this->lowering.generatorLowerer().iteratorFunction(
                                                    call->builtin(), array->type, nullptr, result),
                                                {this->lowering.exprLowerer().emitExpr(array)});
    }
    default: {
      Expr *receiver = cast<MemberExpr>(call->callee)->object;
      Expr *arg = call->args[0];
      llvm::Function *fn = this->lowering.generatorLowerer().iteratorFunction(
          call->builtin(), receiver->type, arg->type, result);
      llvm::Value *source = this->lowering.exprLowerer().emitExpr(receiver);
      return this->lowering.builder_.CreateCall(
          fn, {source, this->lowering.exprLowerer().emitExpr(arg)});
    }
  }
}

}
