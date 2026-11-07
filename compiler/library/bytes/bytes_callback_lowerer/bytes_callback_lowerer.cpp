#include "compiler/library/bytes/bytes_callback_lowerer/bytes_callback_lowerer.h"
#include "compiler/library/bytes/bytes_lowerer/bytes_lowerer.h"
#include "compiler/library/process/process_lowerer/process_lowerer.h"
#include "compiler/library/regex/matcher_builder/matcher_builder.h"
#include "compiler/lower/expr/expr_lowerer/expr_lowerer.h"
#include "compiler/lower/function_frame/function_frame.h"
#include "compiler/lower/functions/closure_lowerer/closure_lowerer.h"
#include "compiler/lower/layouts/type_descriptors/type_descriptors.h"
#include "compiler/lower/memory/ref_counts/ref_counts.h"
#include "compiler/lower/module_builder/module_builder.h"
#include "compiler/lower/types/optional_lowerer/optional_lowerer.h"
#include "compiler/lower/values/array_lowerer/array_lowerer.h"
#include "compiler/lower/values/callback_method_lowerer/callback_method_lowerer.h"
#include "runtime/library/bytes/encodings/encodings.h"
#include "llvm/IR/MDBuilder.h"
#include "llvm/Support/ErrorHandling.h"
#include <numeric>

using llvm::cast;

namespace tz {

BytesCallbackLowerer::BytesCallbackLowerer(Lowering &owner)
    : lowering(owner) {}

llvm::Value *BytesCallbackLowerer::emitBytesCallbackMethod(CallExpr *call) {
  auto *member = cast<MemberExpr>(call->callee);
  const Type *arrayType = member->object->type;
  const Type *element = arrayType->elementType();
  const Type *callbackType = call->args[0]->type;
  llvm::Type *i64 = this->lowering.builder_.getInt64Ty();
  llvm::Type *ptr = this->lowering.builder_.getPtrTy();
  Builtin method = call->builtin();
  bool reduce = method == Builtin::ArrReduce || method == Builtin::ArrReduceRight;
  llvm::Value *array = this->lowering.exprLowerer().emitExpr(member->object);
  llvm::Value *callback = this->lowering.exprLowerer().emitExpr(call->args[0]);
  llvm::Value *init =
      call->args.size() > 1 ? this->lowering.exprLowerer().emitExpr(call->args[1]) : nullptr;

  if (method == Builtin::ArrSortWith) {
    bool copy = member->name == "toSorted";
    llvm::Value *target = array;
    if (copy) {
      target = this->lowering.builder_.CreateCall(
          this->lowering.processLowerer().processFunction(
              "tz_bytes_slice", ptr, {ptr, i64, i64, this->lowering.builder_.getInt32Ty()}),
          {array, this->lowering.builder_.getInt64(0), this->lowering.builder_.getInt64(0),
           this->lowering.builder_.getInt32(0)},
          "sorted");
    }
    llvm::Value *context = this->lowering.functionFrame().spill(callback, "compare");
    this->lowering.builder_.CreateCall(
        this->lowering.processLowerer().processFunction(
            "tz_bytes_sort_with", this->lowering.builder_.getVoidTy(), {ptr, ptr, ptr}),
        {target, this->lowering.callbackMethodLowerer().sortComparator(call), context});
    this->lowering.refCounts().releaseValue(callback, callbackType);
    if (!copy) {
      return array;
    }
    this->lowering.refCounts().releaseValue(array, arrayType);
    return target;
  }

  bool backward = method == Builtin::ArrReduceRight || method == Builtin::ArrFindLast ||
                  method == Builtin::ArrFindLastIndex;
  size_t passes = std::min(callbackType->params().size(), size_t(reduce ? 4 : 3));
  size_t valueArgs = reduce ? 2 : 1;
  llvm::Value *length = this->lowering.bytesLowerer().bytesLength(array);
  llvm::Value *start =
      backward ? this->lowering.builder_.CreateSub(length, this->lowering.builder_.getInt64(1))
               : this->lowering.builder_.getInt64(0);
  llvm::Value *step = this->lowering.builder_.getInt64(backward ? -1 : 1);

  llvm::Value *accSlot = nullptr;
  const Type *accType = reduce && !init ? element : call->type;
  if (reduce) {
    accSlot = this->lowering.functionFrame().createStackSlot(
        this->lowering.moduleBuilder().lower(accType), "acc");
    if (init) {
      this->lowering.builder_.CreateStore(init, accSlot);
    } else {
      start = this->lowering.callbackMethodLowerer().firstOfReduce(
          call, length, start, step, accSlot, accType, [&](llvm::Value *at) {
            return this->lowering.bytesLowerer().loadElement(
                this->lowering.bytesLowerer().bytesData(array), arrayType, at);
          });
    }
  }
  llvm::Value *out = nullptr;
  int32_t kind = int32_t(arrayType->bytes());
  if (method == Builtin::ArrMap) {
    out = this->lowering.builder_.CreateCall(
        this->lowering.processLowerer().processFunction(
            "tz_bytes_new_unset", ptr, {this->lowering.builder_.getInt32Ty(), i64}),
        {this->lowering.builder_.getInt32(uint32_t(kind)), length}, "mapped");
  } else if (method == Builtin::ArrFilter) {
    out = this->lowering.builder_.CreateCall(
        this->lowering.rt_.arrNew,
        {this->lowering.typeDescriptors().typeDescriptor(element),
         this->lowering.builder_.getInt64(0), this->lowering.builder_.getInt64(0)},
        "kept");
  }
  llvm::Value *resultSlot = nullptr;
  if (!reduce && !out && method != Builtin::ArrForEach) {
    resultSlot = this->lowering.functionFrame().createStackSlot(
        this->lowering.moduleBuilder().lower(call->type), "result");
    llvm::Value *none = method == Builtin::ArrSome    ? this->lowering.builder_.getFalse()
                        : method == Builtin::ArrEvery ? this->lowering.builder_.getTrue()
                        : call->type->isInt()
                            ? this->lowering.builder_.getInt64(~uint64_t(0))
                            : this->lowering.optionalLowerer().undefinedValue(call->type);
    this->lowering.builder_.CreateStore(none, resultSlot);
  }

  llvm::Value *indexSlot = this->lowering.functionFrame().createStackSlot(i64, "index");
  this->lowering.builder_.CreateStore(start, indexSlot);
  llvm::BasicBlock *headBlock = this->lowering.functionFrame().newBlock("each");
  llvm::BasicBlock *bodyBlock = this->lowering.functionFrame().newBlock("each.body");
  llvm::BasicBlock *nextBlock = this->lowering.functionFrame().newBlock("each.next");
  llvm::BasicBlock *endBlock = this->lowering.functionFrame().newBlock("each.end");
  this->lowering.functionFrame().emitBlock(headBlock);
  llvm::Value *i = this->lowering.builder_.CreateLoad(i64, indexSlot, "i");
  this->lowering.builder_.CreateCondBr(this->lowering.builder_.CreateICmpULT(i, length), bodyBlock,
                                       endBlock);

  this->lowering.functionFrame().switchTo(bodyBlock);
  llvm::Value *value = this->lowering.bytesLowerer().loadElement(
      this->lowering.bytesLowerer().bytesData(array), arrayType, i);
  std::vector<llvm::Value *> args;
  if (reduce) {
    args.push_back(
        this->lowering.builder_.CreateLoad(this->lowering.moduleBuilder().lower(accType), accSlot));
  }
  args.push_back(value);
  args.push_back(i);
  args.push_back(array);
  if (reduce && passes == 0) {
    this->lowering.refCounts().releaseValue(args[0], accType);
  }
  args.resize(passes);
  if (passes == valueArgs + 2) {
    this->lowering.refCounts().retainValue(array, arrayType);
  }
  llvm::Value *result =
      this->lowering.closureLowerer().callFunctionValue(callback, callbackType, args);
  const Type *resultType = callbackType->ret();

  llvm::BasicBlock *foundBlock = nullptr;
  auto test = [&](llvm::Value *decides) {
    foundBlock = this->lowering.functionFrame().newBlock("each.found");
    llvm::BasicBlock *otherBlock = this->lowering.functionFrame().newBlock("each.other");
    this->lowering.builder_.CreateCondBr(decides, foundBlock, otherBlock);
    this->lowering.functionFrame().switchTo(otherBlock);
  };
  switch (method) {
  case Builtin::ArrForEach:
    if (result) {
      this->lowering.refCounts().releaseValue(result, resultType);
    }
    break;
  case Builtin::ArrMap:
    this->lowering.bytesLowerer().storeElement(this->lowering.bytesLowerer().bytesData(out),
                                               arrayType, i, result);
    break;
  case Builtin::ArrFilter: {
    llvm::BasicBlock *keepBlock = this->lowering.functionFrame().newBlock("filter.keep");
    this->lowering.builder_.CreateCondBr(result, keepBlock, nextBlock);
    this->lowering.functionFrame().switchTo(keepBlock);
    this->lowering.arrayLowerer().emitPush(out, {value}, element);
    break;
  }
  case Builtin::ArrReduce:
  case Builtin::ArrReduceRight:
    this->lowering.builder_.CreateStore(result, accSlot);
    break;
  case Builtin::ArrFind:
  case Builtin::ArrFindLast:
    test(result);
    this->lowering.builder_.CreateBr(nextBlock);
    this->lowering.functionFrame().switchTo(foundBlock);
    this->lowering.builder_.CreateStore(this->lowering.optionalLowerer().makeOptional(
                                            value, this->lowering.builder_.getTrue(), call->type),
                                        resultSlot);
    this->lowering.builder_.CreateBr(endBlock);
    this->lowering.functionFrame().switchTo(this->lowering.functionFrame().newBlock("dead"));
    break;
  case Builtin::ArrFindIndex:
  case Builtin::ArrFindLastIndex:
    test(result);
    this->lowering.builder_.CreateBr(nextBlock);
    this->lowering.functionFrame().switchTo(foundBlock);
    this->lowering.builder_.CreateStore(i, resultSlot);
    this->lowering.builder_.CreateBr(endBlock);
    this->lowering.functionFrame().switchTo(this->lowering.functionFrame().newBlock("dead"));
    break;
  case Builtin::ArrSome:
  case Builtin::ArrEvery: {
    bool some = method == Builtin::ArrSome;
    test(some ? result : this->lowering.builder_.CreateNot(result));
    this->lowering.builder_.CreateBr(nextBlock);
    this->lowering.functionFrame().switchTo(foundBlock);
    this->lowering.builder_.CreateStore(this->lowering.builder_.getInt1(some), resultSlot);
    this->lowering.builder_.CreateBr(endBlock);
    this->lowering.functionFrame().switchTo(this->lowering.functionFrame().newBlock("dead"));
    break;
  }
  default:
    llvm_unreachable("not a method with a callback");
  }
  this->lowering.functionFrame().branchTo(nextBlock);

  this->lowering.functionFrame().switchTo(nextBlock);
  this->lowering.builder_.CreateStore(this->lowering.builder_.CreateAdd(i, step), indexSlot);
  this->lowering.builder_.CreateBr(headBlock);

  this->lowering.functionFrame().switchTo(endBlock);
  this->lowering.refCounts().releaseValue(callback, callbackType);
  this->lowering.refCounts().releaseValue(array, arrayType);
  if (method == Builtin::ArrMap) {
    return out;
  }
  if (method == Builtin::ArrFilter) {
    llvm::Value *kept = out;
    llvm::Value *made = this->lowering.builder_.CreateCall(
        this->lowering.processLowerer().processFunction(
            "tz_bytes_from_array", ptr, {this->lowering.builder_.getInt32Ty(), ptr}),
        {this->lowering.builder_.getInt32(uint32_t(kind)), kept}, "filtered");
    this->lowering.refCounts().releaseValue(kept, Type::array(element));
    return made;
  }
  if (reduce) {
    return this->lowering.callbackMethodLowerer().reduceResult(call, length, accSlot, accType);
  }
  if (resultSlot) {
    return this->lowering.builder_.CreateLoad(this->lowering.moduleBuilder().lower(call->type),
                                              resultSlot);
  }

  return nullptr;
}

}
