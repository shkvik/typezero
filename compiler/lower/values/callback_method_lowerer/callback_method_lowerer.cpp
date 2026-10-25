#include "compiler/lower/values/callback_method_lowerer/callback_method_lowerer.h"
#include "compiler/lower/module_builder/module_builder.h"
#include "compiler/lower/function_builder/function_builder.h"
#include "compiler/lower/function_frame/function_frame.h"
#include "compiler/lower/memory/ref_counts/ref_counts.h"
#include "compiler/lower/types/optional_lowerer/optional_lowerer.h"
#include "compiler/lower/expr/expr_lowerer/expr_lowerer.h"
#include "compiler/lower/values/array_lowerer/array_lowerer.h"
#include "compiler/lower/functions/closure_lowerer/closure_lowerer.h"
#include "compiler/lower/memory/group_lowerer/group_lowerer.h"
#include "compiler/lower/errors/may_throw/may_throw.h"
#include "compiler/lower/errors/throw_lowerer/throw_lowerer.h"
#include "compiler/library/bytes/bytes_callback_lowerer/bytes_callback_lowerer.h"
#include "compiler/library/decimal/decimal_lowerer/decimal_lowerer.h"
#include "compiler/library/regex/matcher_builder/matcher_builder.h"
#include "llvm/IR/MDBuilder.h"
#include "llvm/Support/ErrorHandling.h"

using llvm::cast;
using llvm::dyn_cast;
using llvm::isa;
using llvm::BasicBlock;
using llvm::Value;

namespace tz {

namespace {

// Header fields; must match runtime/core/array/array.c.
enum ArrayField : unsigned { kRefCount, kElementType, kData, kLength, kCapacity, kAlloc };

} // namespace

CallbackMethodLowerer::CallbackMethodLowerer(CodeGen &codegen) : codegen_(codegen) {}

// forEach, map, filter, flatMap, reduce, reduceRight, find, findIndex,
// findLast, findLastIndex, some and every. The array is evaluated once and
// stays alive during the call, and the callback gets that array, whatever it
// does meanwhile. As in JavaScript, the elements visited are those below the
// length the array had at the start that are still there when their turn
// comes; elements the callback adds are not visited.
llvm::Value *CallbackMethodLowerer::emitCallbackMethod(CallExpr *call) {
  auto *member = cast<MemberExpr>(call->callee);
  if (member->object->type->isTypedArray()) {
    return codegen_.bytesCallbackLowerer().emitBytesCallbackMethod(call);
  }
  const Type *arrayType = member->object->type;
  const Type *element = arrayType->inner();
  const Type *callbackType = call->args[0]->type;
  llvm::Type *i64 = codegen_.builder_.getInt64Ty();
  Builtin method = call->builtin;

  // The array and the callback wait in slots while the callback may throw,
  // as does what the method builds, so that the way out releases them.
  bool throws = codegen_.typeThrows(callbackType);
  // reduce of an empty array without an initial value throws too.
  bool emptyThrows =
      (method == Builtin::ArrReduce || method == Builtin::ArrReduceRight) && call->args.size() < 2;
  llvm::Value *array = codegen_.exprLowerer().emitExpr(member->object);
  CodeGen::Held heldArray = codegen_.throwLowerer().hold(array, arrayType,
                        throws || emptyThrows ||
                            llvm::any_of(call->args, [&](Expr *arg) { return codegen_.mayThrow().exprThrows(arg); }));
  llvm::Value *callback = codegen_.exprLowerer().emitExpr(call->args[0]);
  CodeGen::Held heldCallback =
      codegen_.throwLowerer().hold(callback, callbackType,
           throws || emptyThrows || (call->args.size() > 1 && codegen_.mayThrow().exprThrows(call->args[1])));
  llvm::Value *init = call->args.size() > 1 ? codegen_.exprLowerer().emitExpr(call->args[1]) : nullptr;

  if (method == Builtin::ArrSortWith) {
    // The runtime sorts; the comparison goes through a function that calls
    // the callback, and does nothing more once it threw.
    llvm::Value *context = codegen_.functionFrame().spill(callback, "compare");
    codegen_.builder_.CreateCall(codegen_.rt_.arrSortWith, {array, sortComparator(call), context});
    codegen_.refCounts().releaseValue(codegen_.throwLowerer().take(heldCallback), callbackType);
    codegen_.emitThrowCheckIf(throws);
    return codegen_.throwLowerer().take(heldArray); // the same array, with this reference
  }

  bool reduce = method == Builtin::ArrReduce || method == Builtin::ArrReduceRight;
  bool backward = method == Builtin::ArrReduceRight || method == Builtin::ArrFindLast ||
                  method == Builtin::ArrFindLastIndex;
  size_t passes = std::min(callbackType->params().size(), size_t(reduce ? 4 : 3));
  // The arguments of the callback that are the element (and the accumulator).
  size_t valueArgs = reduce ? 2 : 1;

  llvm::Value *length = codegen_.arrayLowerer().arrayLength(array);
  llvm::Value *start =
      backward ? codegen_.builder_.CreateSub(length, codegen_.builder_.getInt64(1)) : codegen_.builder_.getInt64(0);
  llvm::Value *step = codegen_.builder_.getInt64(backward ? -1 : 1);
  auto readElement = [&](llvm::Value *at) {
    return codegen_.builder_.CreateLoad(codegen_.moduleBuilder().lower(element), codegen_.arrayLowerer().elementAddress(codegen_.arrayLowerer().arrayData(array), element, at),
                               "element");
  };

  // reduce: the accumulator, from the initial value or the first element.
  llvm::Value *accSlot = nullptr;
  if (reduce) {
    accSlot = codegen_.functionFrame().createStackSlot(codegen_.moduleBuilder().lower(call->type), "acc");
    if (init) {
      codegen_.builder_.CreateStore(init, accSlot);
    } else {
      llvm::BasicBlock *emptyBlock = codegen_.functionFrame().newBlock("reduce.empty");
      llvm::BasicBlock *firstBlock = codegen_.functionFrame().newBlock("reduce.first");
      codegen_.builder_.CreateCondBr(codegen_.builder_.CreateICmpEQ(length, codegen_.builder_.getInt64(0)), emptyBlock,
                            firstBlock, llvm::MDBuilder(codegen_.ctx_).createUnlikelyBranchWeights());
      codegen_.functionFrame().switchTo(emptyBlock);
      codegen_.throwLowerer().emitThrowNew(1, "Reduce of empty array with no initial value");
      codegen_.functionFrame().switchTo(firstBlock);
      llvm::Value *first = readElement(start);
      codegen_.refCounts().retainValue(first, element);
      codegen_.builder_.CreateStore(first, accSlot);
      start = codegen_.builder_.CreateAdd(start, step);
    }
  }
  // map, filter, flatMap: the new array.
  llvm::Value *out = nullptr;
  if (method == Builtin::ArrMap || method == Builtin::ArrFilter || method == Builtin::ArrFlatMap) {
    out = codegen_.groupLowerer().newArray(call->type, codegen_.builder_.getInt64(0),
                   method == Builtin::ArrMap ? length : codegen_.builder_.getInt64(0),
                   codegen_.makesMember(call, call->type), "result");
  }
  if (out) {
    codegen_.groupLowerer().noteUnstored(out, call);
  }
  CodeGen::Held heldOut = codegen_.throwLowerer().hold(out, call->type, throws);
  // find, findIndex, some, every: the result if no element decides it.
  llvm::Value *resultSlot = nullptr;
  if (!reduce && !out && method != Builtin::ArrForEach) {
    resultSlot = codegen_.functionFrame().createStackSlot(codegen_.moduleBuilder().lower(call->type), "result");
    llvm::Value *none = method == Builtin::ArrSome    ? codegen_.builder_.getFalse()
                        : method == Builtin::ArrEvery ? codegen_.builder_.getTrue()
                        : call->type->isInt()         ? codegen_.builder_.getInt32(~0u)
                                                      : codegen_.optionalLowerer().undefinedValue(call->type);
    codegen_.builder_.CreateStore(none, resultSlot);
  }

  llvm::Value *indexSlot = codegen_.functionFrame().createStackSlot(i64, "index");
  codegen_.builder_.CreateStore(start, indexSlot);
  llvm::BasicBlock *headBlock = codegen_.functionFrame().newBlock("each");
  llvm::BasicBlock *bodyBlock = codegen_.functionFrame().newBlock("each.body");
  llvm::BasicBlock *nextBlock = codegen_.functionFrame().newBlock("each.next");
  llvm::BasicBlock *endBlock = codegen_.functionFrame().newBlock("each.end");
  codegen_.functionFrame().emitBlock(headBlock);
  llvm::Value *i = codegen_.builder_.CreateLoad(i64, indexSlot, "i");
  // Below the length at the start (or not below 0 going back), and still in
  // the array. Going back, the unsigned comparison covers both.
  llvm::Value *inRange = codegen_.builder_.CreateICmpULT(i, codegen_.arrayLowerer().arrayLength(array));
  if (!backward) {
    inRange = codegen_.builder_.CreateAnd(inRange, codegen_.builder_.CreateICmpULT(i, length));
  }
  codegen_.builder_.CreateCondBr(inRange, bodyBlock, endBlock);

  codegen_.functionFrame().switchTo(bodyBlock);
  llvm::Value *value = readElement(i);
  // The callback owns its arguments; filter and find keep one more
  // reference to the element, to put it in the result.
  bool keeps =
      method == Builtin::ArrFilter || method == Builtin::ArrFind || method == Builtin::ArrFindLast;
  if (passes >= valueArgs) {
    codegen_.refCounts().retainValue(value, element);
  }
  if (keeps) {
    codegen_.refCounts().retainValue(value, element);
  }
  std::vector<llvm::Value *> args;
  if (reduce) {
    args.push_back(codegen_.builder_.CreateLoad(codegen_.moduleBuilder().lower(call->type), accSlot)); // moves into the call
  }
  args.push_back(value);
  args.push_back(codegen_.builder_.CreateTrunc(i, codegen_.builder_.getInt32Ty()));
  args.push_back(array);
  // The accumulator moves into the call, unless the callback takes none.
  if (reduce && passes == 0) {
    codegen_.refCounts().releaseValue(args[0], call->type);
  }
  args.resize(passes);
  if (passes == valueArgs + 2) {
    codegen_.refCounts().retainValue(array, arrayType);
  }
  CodeGen::Held heldElement = codegen_.throwLowerer().hold(value, element, throws && keeps);
  llvm::Value *result = codegen_.closureLowerer().callFunctionValue(callback, callbackType, args);
  codegen_.emitThrowCheckIf(throws);
  codegen_.throwLowerer().take(heldElement);
  const Type *resultType = callbackType->ret();

  // What the result means. `found` is where an element decides the result.
  llvm::BasicBlock *foundBlock = nullptr;
  auto test = [&](llvm::Value *decides) {
    foundBlock = codegen_.functionFrame().newBlock("each.found");
    llvm::BasicBlock *otherBlock = codegen_.functionFrame().newBlock("each.other");
    codegen_.builder_.CreateCondBr(decides, foundBlock, otherBlock);
    codegen_.functionFrame().switchTo(otherBlock);
  };
  switch (method) {
  case Builtin::ArrForEach:
    if (result) {
      codegen_.refCounts().releaseValue(result, resultType);
    }
    break;
  case Builtin::ArrMap:
    // One result per element visited, in order: the new array has room for
    // them all.
    codegen_.builder_.CreateStore(result, codegen_.arrayLowerer().elementAddress(codegen_.arrayLowerer().arrayData(out), resultType, i));
    codegen_.builder_.CreateStore(
        codegen_.builder_.CreateTrunc(codegen_.builder_.CreateAdd(i, codegen_.builder_.getInt64(1)), codegen_.builder_.getInt32Ty()),
        codegen_.arrayLowerer().arrayField(out, kLength));
    break;
  case Builtin::ArrFlatMap:
    if (resultType->isArray()) {
      codegen_.builder_.CreateCall(codegen_.rt_.arrAppend, {out, result});
      codegen_.refCounts().releaseValue(result, resultType);
    } else {
      codegen_.arrayLowerer().emitPush(out, {result}, resultType);
    }
    break;
  case Builtin::ArrFilter: {
    llvm::BasicBlock *keepBlock = codegen_.functionFrame().newBlock("filter.keep");
    llvm::BasicBlock *dropBlock = codegen_.functionFrame().newBlock("filter.drop");
    codegen_.builder_.CreateCondBr(result, keepBlock, dropBlock);
    codegen_.functionFrame().switchTo(keepBlock);
    // `filter(Boolean)` of `T | undefined` elements keeps those that hold a
    // value, as plain T.
    const Type *kept = call->type->inner();
    codegen_.arrayLowerer().emitPush(out, {codegen_.exprLowerer().emitConversion(value, element, kept)}, kept);
    codegen_.builder_.CreateBr(nextBlock);
    codegen_.functionFrame().switchTo(dropBlock);
    codegen_.refCounts().releaseValue(value, element);
    break;
  }
  case Builtin::ArrReduce:
  case Builtin::ArrReduceRight:
    codegen_.builder_.CreateStore(result, accSlot);
    break;
  case Builtin::ArrFind:
  case Builtin::ArrFindLast:
    test(result);
    codegen_.refCounts().releaseValue(value, element);
    codegen_.builder_.CreateBr(nextBlock);
    codegen_.functionFrame().switchTo(foundBlock);
    codegen_.builder_.CreateStore(codegen_.optionalLowerer().makeOptional(value, codegen_.builder_.getTrue(), call->type), resultSlot);
    codegen_.builder_.CreateBr(endBlock);
    codegen_.functionFrame().switchTo(codegen_.functionFrame().newBlock("dead"));
    break;
  case Builtin::ArrFindIndex:
  case Builtin::ArrFindLastIndex:
    test(result);
    codegen_.builder_.CreateBr(nextBlock);
    codegen_.functionFrame().switchTo(foundBlock);
    codegen_.builder_.CreateStore(codegen_.builder_.CreateTrunc(i, codegen_.builder_.getInt32Ty()), resultSlot);
    codegen_.builder_.CreateBr(endBlock);
    codegen_.functionFrame().switchTo(codegen_.functionFrame().newBlock("dead"));
    break;
  case Builtin::ArrSome:
  case Builtin::ArrEvery: {
    bool some = method == Builtin::ArrSome;
    test(some ? result : codegen_.builder_.CreateNot(result));
    codegen_.builder_.CreateBr(nextBlock);
    codegen_.functionFrame().switchTo(foundBlock);
    codegen_.builder_.CreateStore(codegen_.builder_.getInt1(some), resultSlot);
    codegen_.builder_.CreateBr(endBlock);
    codegen_.functionFrame().switchTo(codegen_.functionFrame().newBlock("dead"));
    break;
  }
  default:
    llvm_unreachable("not a method with a callback");
  }
  codegen_.functionFrame().branchTo(nextBlock);

  codegen_.functionFrame().switchTo(nextBlock);
  codegen_.builder_.CreateStore(codegen_.builder_.CreateAdd(i, step), indexSlot);
  codegen_.builder_.CreateBr(headBlock);

  codegen_.functionFrame().switchTo(endBlock);
  codegen_.refCounts().releaseValue(codegen_.throwLowerer().take(heldCallback), callbackType);
  codegen_.refCounts().releaseValue(codegen_.throwLowerer().take(heldArray), arrayType);
  if (out) {
    return codegen_.throwLowerer().take(heldOut);
  }
  if (reduce) {
    return codegen_.builder_.CreateLoad(codegen_.moduleBuilder().lower(call->type), accSlot);
  }
  if (resultSlot) {
    return codegen_.builder_.CreateLoad(codegen_.moduleBuilder().lower(call->type), resultSlot);
  }

  return nullptr;
}

// The function the runtime calls to compare two elements for sort(compare):
// (ptr context, ptr x, ptr y) -> i32, the sign of compare(*x, *y), where
// the context holds the function value. NaN counts as 0.
llvm::Function *CallbackMethodLowerer::sortComparator(CallExpr *call) {
  const Type *receiver = cast<MemberExpr>(call->callee)->object->type;
  // The runtime gives the elements of a typed array as numbers of their type.
  const Type *element = receiver->isTypedArray() ? receiver->elementType() : receiver->inner();
  Expr *callbackExpr = call->args[0];
  const Type *callbackType = callbackExpr->type;
  llvm::Type *ptr = codegen_.builder_.getPtrTy();
  auto *type = llvm::FunctionType::get(codegen_.builder_.getInt32Ty(), {ptr, ptr, ptr}, false);
  auto *fn =
      llvm::Function::Create(type, llvm::GlobalValue::InternalLinkage, "tz.compare", codegen_.module_.get());
  codegen_.functionBuilder().setFunctionAttributes(fn);
  llvm::IRBuilderBase::InsertPoint point = codegen_.builder_.saveIP();
  llvm::Function *outer = codegen_.function_;
  codegen_.function_ = fn;
  codegen_.builder_.SetInsertPoint(llvm::BasicBlock::Create(codegen_.ctx_, "entry", fn));
  if (codegen_.typeThrows(callbackType)) {
    // Once the callback threw, the sort only has to end.
    llvm::BasicBlock *thrownBlock = llvm::BasicBlock::Create(codegen_.ctx_, "thrown", fn);
    llvm::BasicBlock *compareBlock = llvm::BasicBlock::Create(codegen_.ctx_, "compare", fn);
    codegen_.builder_.CreateCondBr(
        codegen_.builder_.CreateIsNotNull(codegen_.builder_.CreateLoad(codegen_.builder_.getPtrTy(), codegen_.throwLowerer().thrownSlot())),
        thrownBlock, compareBlock);
    codegen_.builder_.SetInsertPoint(thrownBlock);
    codegen_.builder_.CreateRet(codegen_.builder_.getInt32(0));
    codegen_.builder_.SetInsertPoint(compareBlock);
  }
  llvm::Value *callback = codegen_.builder_.CreateLoad(codegen_.fnTy_, fn->getArg(0), "compare");
  size_t passes = std::min<size_t>(callbackType->params().size(), 2);
  std::vector<llvm::Value *> args;
  for (size_t k = 0; k < passes; ++k) {
    llvm::Value *value = codegen_.builder_.CreateLoad(codegen_.moduleBuilder().lower(element), fn->getArg(unsigned(k + 1)));
    codegen_.refCounts().retainValue(value, element); // the callback owns its arguments
    args.push_back(value);
  }
  llvm::Value *result;
  if (auto *fe = dyn_cast<FunctionExpr>(callbackExpr)) {
    // An arrow written here: its code is known, and may be inlined.
    std::vector<llvm::Value *> all{codegen_.builder_.CreateExtractValue(callback, 1)};
    all.insert(all.end(), args.begin(), args.end());
    for (size_t k = passes; k < callbackType->params().size(); ++k) {
      all.push_back(codegen_.optionalLowerer().undefinedValue(callbackType->params()[k]));
    }
    result = codegen_.builder_.CreateCall(codegen_.closureLowerer().closureCode(fe->fn), all);
  } else {
    result = codegen_.closureLowerer().callFunctionValue(callback, callbackType, args);
  }
  const Type *resultType = callbackType->ret();
  if (resultType->isDecimal()) {
    llvm::Value *sign = codegen_.decimalLowerer().decimalSign(result);
    codegen_.refCounts().releaseValue(result, resultType);
    codegen_.builder_.CreateRet(sign);
    codegen_.function_ = outer;
    codegen_.builder_.restoreIP(point);
    return fn;
  }
  llvm::Value *positive, *negative;
  if (resultType->isFloat()) {
    llvm::Value *zero = llvm::ConstantFP::get(codegen_.builder_.getDoubleTy(), 0.0);
    positive = codegen_.builder_.CreateFCmpOGT(result, zero);
    negative = codegen_.builder_.CreateFCmpOLT(result, zero);
  } else {
    llvm::Value *zero = llvm::ConstantInt::get(result->getType(), 0);
    positive = codegen_.builder_.CreateICmpSGT(result, zero);
    negative = codegen_.builder_.CreateICmpSLT(result, zero);
  }
  codegen_.builder_.CreateRet(codegen_.builder_.CreateSub(codegen_.builder_.CreateZExt(positive, codegen_.builder_.getInt32Ty()),
                                        codegen_.builder_.CreateZExt(negative, codegen_.builder_.getInt32Ty())));
  codegen_.function_ = outer;
  codegen_.builder_.restoreIP(point);

  return fn;
}

} // namespace tz
