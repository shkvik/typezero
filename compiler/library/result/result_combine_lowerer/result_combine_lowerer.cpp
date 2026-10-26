#include "compiler/library/result/result_combine_lowerer/result_combine_lowerer.h"
#include "compiler/library/result/result_lowerer/result_lowerer.h"
#include "compiler/lower/expr/expr_lowerer/expr_lowerer.h"
#include "compiler/lower/function_frame/function_frame.h"
#include "compiler/lower/layouts/object_layouts/object_layouts.h"
#include "compiler/lower/memory/group_lowerer/group_lowerer.h"
#include "compiler/lower/memory/ref_counts/ref_counts.h"
#include "compiler/lower/module_builder/module_builder.h"
#include "compiler/lower/values/array_lowerer/array_lowerer.h"
#include "llvm/IR/MDBuilder.h"
#include <functional>

using llvm::BasicBlock;
using llvm::cast;
using llvm::dyn_cast;
using llvm::isa;
using llvm::Value;

namespace tz {

ResultCombineLowerer::ResultCombineLowerer(Lowering &lowering) : lowering_(lowering) {}

llvm::Value *ResultCombineLowerer::emitCombine(CallExpr *call) {
  if (call->args[0]->type->isTuple()) {
    return emitCombineTuple(call);
  }
  bool all = call->builtin == Builtin::ResultCombineWithAllErrors;
  const Type *arrayType = call->args[0]->type, *element = arrayType->inner();
  const Type *valueType = element->okType(), *errorType = element->errType();
  const Type *resultType = call->type;
  llvm::Type *i64 = lowering_.builder_.getInt64Ty();

  Lowering::Operand array = lowering_.refCounts().emitOperand(call->args[0], true);
  llvm::Value *length = lowering_.arrayLowerer().arrayLength(array.value);
  llvm::Value *values = lowering_.groupLowerer().newArray(
      Type::array(valueType), lowering_.builder_.getInt64(0), length, "values");
  bool collects = all && !errorType->isNever();
  llvm::Value *errors = collects ? lowering_.groupLowerer().newArray(
                                       Type::array(errorType), lowering_.builder_.getInt64(0),
                                       lowering_.builder_.getInt64(0), "errors")
                                 : nullptr;
  llvm::Value *resultSlot = lowering_.functionFrame().createStackSlot(
      lowering_.moduleBuilder().lower(resultType), "combined");

  llvm::Value *indexSlot = lowering_.functionFrame().createStackSlot(i64, "index");
  lowering_.builder_.CreateStore(lowering_.builder_.getInt64(0), indexSlot);
  llvm::BasicBlock *headBlock = lowering_.functionFrame().newBlock("combine");
  llvm::BasicBlock *bodyBlock = lowering_.functionFrame().newBlock("combine.body");
  llvm::BasicBlock *okBlock = lowering_.functionFrame().newBlock("combine.ok");
  llvm::BasicBlock *errBlock = lowering_.functionFrame().newBlock("combine.err");
  llvm::BasicBlock *nextBlock = lowering_.functionFrame().newBlock("combine.next");
  llvm::BasicBlock *endBlock = lowering_.functionFrame().newBlock("combine.end");
  llvm::BasicBlock *doneBlock = lowering_.functionFrame().newBlock("combine.done");
  lowering_.functionFrame().emitBlock(headBlock);
  llvm::Value *i = lowering_.builder_.CreateLoad(i64, indexSlot, "i");
  lowering_.builder_.CreateCondBr(lowering_.builder_.CreateICmpULT(i, length), bodyBlock, endBlock);

  lowering_.functionFrame().switchTo(bodyBlock);
  llvm::Value *result = lowering_.builder_.CreateLoad(
      lowering_.moduleBuilder().lower(element),
      lowering_.arrayLowerer().elementAddress(lowering_.arrayLowerer().arrayData(array.value),
                                              element, i),
      "result");
  lowering_.builder_.CreateCondBr(lowering_.resultLowerer().resultIsOk(result, element), okBlock,
                                  errBlock);

  lowering_.functionFrame().switchTo(okBlock);
  llvm::Value *value = lowering_.resultLowerer().resultHalf(result, element, false);
  lowering_.refCounts().retainValue(value, valueType);
  lowering_.arrayLowerer().emitPush(values, {value}, valueType);
  lowering_.builder_.CreateBr(nextBlock);

  lowering_.functionFrame().switchTo(errBlock);
  llvm::Value *error = lowering_.resultLowerer().resultHalf(result, element, true);
  lowering_.refCounts().retainValue(error, errorType);
  if (all) {
    if (collects) {
      lowering_.arrayLowerer().emitPush(errors, {error}, errorType);
    }
    lowering_.builder_.CreateBr(nextBlock);
  } else {
    lowering_.refCounts().releaseValue(values, Type::array(valueType));
    lowering_.builder_.CreateStore(lowering_.resultLowerer().makeResult(
                                       resultType, lowering_.builder_.getFalse(), nullptr, error),
                                   resultSlot);
    lowering_.builder_.CreateBr(doneBlock);
  }

  lowering_.functionFrame().switchTo(nextBlock);
  lowering_.builder_.CreateStore(lowering_.builder_.CreateAdd(i, lowering_.builder_.getInt64(1)),
                                 indexSlot);
  lowering_.builder_.CreateBr(headBlock);

  lowering_.functionFrame().switchTo(endBlock);
  if (!collects) {
    lowering_.builder_.CreateStore(lowering_.resultLowerer().makeResult(
                                       resultType, lowering_.builder_.getTrue(), values, nullptr),
                                   resultSlot);
  } else {
    llvm::Value *failed = lowering_.builder_.CreateICmpNE(
        lowering_.arrayLowerer().arrayLength(errors), lowering_.builder_.getInt64(0));
    llvm::Value *okResult = lowering_.resultLowerer().makeResult(
        resultType, lowering_.builder_.getTrue(), values, nullptr);
    llvm::Value *errResult = lowering_.resultLowerer().makeResult(
        resultType, lowering_.builder_.getFalse(), nullptr, errors);
    llvm::BasicBlock *failBlock = lowering_.functionFrame().newBlock("combine.failed");
    llvm::BasicBlock *passBlock = lowering_.functionFrame().newBlock("combine.passed");
    lowering_.builder_.CreateCondBr(failed, failBlock, passBlock);
    lowering_.functionFrame().switchTo(failBlock);
    lowering_.refCounts().releaseValue(values, Type::array(valueType));
    lowering_.builder_.CreateStore(errResult, resultSlot);
    lowering_.builder_.CreateBr(doneBlock);
    lowering_.functionFrame().switchTo(passBlock);
    lowering_.refCounts().releaseValue(errors, resultType->errType());
    lowering_.builder_.CreateStore(okResult, resultSlot);
  }
  lowering_.functionFrame().emitBlock(doneBlock);
  lowering_.refCounts().dropOperand(array);

  return lowering_.builder_.CreateLoad(lowering_.moduleBuilder().lower(resultType), resultSlot);
}

llvm::Value *ResultCombineLowerer::emitCombineTuple(CallExpr *call) {
  bool all = call->builtin == Builtin::ResultCombineWithAllErrors;
  auto *list = cast<ArrayLitExpr>(call->args[0]);
  const Type *resultType = call->type, *tupleType = resultType->okType();
  const Type *errorType = all && resultType->errType()->isArray() ? resultType->errType()->inner()
                                                                  : resultType->errType();
  std::vector<llvm::Value *> results;
  std::vector<const Type *> types;
  for (const ArrayLitExpr::Element &e : list->elements) {
    results.push_back(lowering_.exprLowerer().emitExpr(e.expr));
    types.push_back(e.expr->type);
  }
  llvm::Value *resultSlot = lowering_.functionFrame().createStackSlot(
      lowering_.moduleBuilder().lower(resultType), "combined");
  llvm::BasicBlock *doneBlock = lowering_.functionFrame().newBlock("combine.done");
  llvm::Value *errors = all && !errorType->isNever()
                            ? lowering_.groupLowerer().newArray(
                                  Type::array(errorType), lowering_.builder_.getInt64(0),
                                  lowering_.builder_.getInt64(0), "errors")
                            : nullptr;

  for (size_t i = 0; i < results.size(); ++i) {
    if (types[i]->errType()->isNever()) {
      continue;
    }
    llvm::BasicBlock *errBlock = lowering_.functionFrame().newBlock("combine.err");
    llvm::BasicBlock *nextBlock = lowering_.functionFrame().newBlock("combine.next");
    lowering_.builder_.CreateCondBr(lowering_.resultLowerer().resultIsOk(results[i], types[i]),
                                    nextBlock, errBlock);
    lowering_.functionFrame().switchTo(errBlock);
    llvm::Value *error = lowering_.resultLowerer().convertHalf(
        lowering_.resultLowerer().resultHalf(results[i], types[i], true), types[i]->errType(),
        errorType);
    if (errors) {
      lowering_.arrayLowerer().emitPush(errors, {error}, errorType);
    } else {
      for (size_t j = 0; j < results.size(); ++j) {
        if (j != i) {
          lowering_.refCounts().releaseValue(results[j], types[j]);
        }
      }
      lowering_.builder_.CreateStore(lowering_.resultLowerer().makeResult(
                                         resultType, lowering_.builder_.getFalse(), nullptr, error),
                                     resultSlot);
      lowering_.builder_.CreateBr(doneBlock);
      lowering_.functionFrame().switchTo(nextBlock);
      continue;
    }
    lowering_.functionFrame().emitBlock(nextBlock);
  }

  auto makeTuple = [&] {
    const Lowering::ObjectLayout &layout = lowering_.objectLayouts().objectLayout(tupleType);
    std::vector<unsigned> keys;
    for (unsigned i = 0; i < results.size(); ++i) {
      keys.push_back(i);
    }
    llvm::Value *tuple = lowering_.objectLayouts().allocObject(tupleType, keys);
    for (unsigned i = 0; i < results.size(); ++i) {
      lowering_.builder_.CreateStore(
          lowering_.resultLowerer().resultHalf(results[i], types[i], false),
          lowering_.builder_.CreateStructGEP(layout.type, tuple, layout.elements[i]));
    }
    lowering_.groupLowerer().adoptFields(tuple, tupleType);
    lowering_.builder_.CreateStore(lowering_.resultLowerer().makeResult(
                                       resultType, lowering_.builder_.getTrue(), tuple, nullptr),
                                   resultSlot);
  };
  if (!errors) {
    makeTuple();
  } else {
    llvm::BasicBlock *failBlock = lowering_.functionFrame().newBlock("combine.failed");
    llvm::BasicBlock *passBlock = lowering_.functionFrame().newBlock("combine.passed");
    lowering_.builder_.CreateCondBr(
        lowering_.builder_.CreateICmpNE(lowering_.arrayLowerer().arrayLength(errors),
                                        lowering_.builder_.getInt64(0)),
        failBlock, passBlock);
    lowering_.functionFrame().switchTo(failBlock);
    for (size_t i = 0; i < results.size(); ++i) {
      lowering_.refCounts().releaseValue(
          lowering_.resultLowerer().resultHalf(results[i], types[i], false), types[i]->okType());
    }
    lowering_.builder_.CreateStore(lowering_.resultLowerer().makeResult(
                                       resultType, lowering_.builder_.getFalse(), nullptr, errors),
                                   resultSlot);
    lowering_.builder_.CreateBr(doneBlock);
    lowering_.functionFrame().switchTo(passBlock);
    lowering_.refCounts().releaseValue(errors, resultType->errType());
    makeTuple();
  }
  lowering_.functionFrame().emitBlock(doneBlock);

  return lowering_.builder_.CreateLoad(lowering_.moduleBuilder().lower(resultType), resultSlot);
}

}
