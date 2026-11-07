#include "compiler/library/result/result_combine_lowerer/result_combine_lowerer.h"
#include "compiler/library/result/err_sites/err_sites.h"
#include "compiler/library/result/result_lowerer/result_lowerer.h"
#include "compiler/lower/expr/expr_lowerer/expr_lowerer.h"
#include "compiler/lower/function_frame/function_frame.h"
#include "compiler/lower/layouts/object_layouts/object_layouts.h"
#include "compiler/lower/memory/group_lowerer/group_lowerer.h"
#include "compiler/lower/memory/ref_counts/ref_counts.h"
#include "compiler/lower/module_builder/module_builder.h"
#include "compiler/lower/values/array_lowerer/array_lowerer.h"
#include "llvm/IR/MDBuilder.h"

using llvm::cast;

namespace tz {

ResultCombineLowerer::ResultCombineLowerer(Lowering &owner)
    : lowering(owner) {}

llvm::Value *ResultCombineLowerer::emitCombine(CallExpr *call) {
  if (call->args[0]->type->isTuple()) {
    return this->emitCombineTuple(call);
  }
  bool all = call->builtin() == Builtin::ResultCombineWithAllErrors;
  const Type *arrayType = call->args[0]->type;
  const Type *element = arrayType->inner();
  const Type *valueType = element->okType();
  const Type *errorType = element->errType();
  const Type *resultType = call->type;
  llvm::Type *i64 = this->lowering.builder_.getInt64Ty();

  Lowering::Operand array = this->lowering.refCounts().emitOperand(call->args[0], true);
  llvm::Value *length = this->lowering.arrayLowerer().arrayLength(array.value);
  llvm::Value *values = this->lowering.groupLowerer().newArray(
      Type::array(valueType), this->lowering.builder_.getInt64(0), length, "values");
  bool collects = all && !errorType->isNever();
  llvm::Value *errors = collects ? this->lowering.groupLowerer().newArray(
                                       Type::array(errorType), this->lowering.builder_.getInt64(0),
                                       this->lowering.builder_.getInt64(0), "errors")
                                 : nullptr;
  llvm::Value *resultSlot = this->lowering.functionFrame().createStackSlot(
      this->lowering.moduleBuilder().lower(resultType), "combined");

  llvm::Value *indexSlot = this->lowering.functionFrame().createStackSlot(i64, "index");
  this->lowering.builder_.CreateStore(this->lowering.builder_.getInt64(0), indexSlot);
  llvm::BasicBlock *headBlock = this->lowering.functionFrame().newBlock("combine");
  llvm::BasicBlock *bodyBlock = this->lowering.functionFrame().newBlock("combine.body");
  llvm::BasicBlock *okBlock = this->lowering.functionFrame().newBlock("combine.ok");
  llvm::BasicBlock *errBlock = this->lowering.functionFrame().newBlock("combine.err");
  llvm::BasicBlock *nextBlock = this->lowering.functionFrame().newBlock("combine.next");
  llvm::BasicBlock *endBlock = this->lowering.functionFrame().newBlock("combine.end");
  llvm::BasicBlock *doneBlock = this->lowering.functionFrame().newBlock("combine.done");
  this->lowering.functionFrame().emitBlock(headBlock);
  llvm::Value *i = this->lowering.builder_.CreateLoad(i64, indexSlot, "i");
  this->lowering.builder_.CreateCondBr(this->lowering.builder_.CreateICmpULT(i, length), bodyBlock,
                                       endBlock);

  this->lowering.functionFrame().switchTo(bodyBlock);
  llvm::Value *result = this->lowering.builder_.CreateLoad(
      this->lowering.moduleBuilder().lower(element),
      this->lowering.arrayLowerer().elementAddress(
          this->lowering.arrayLowerer().arrayData(array.value), element, i),
      "result");
  this->lowering.builder_.CreateCondBr(this->lowering.resultLowerer().resultIsOk(result, element),
                                       okBlock, errBlock);

  this->lowering.functionFrame().switchTo(okBlock);
  llvm::Value *value = this->lowering.resultLowerer().resultHalf(result, element, false);
  this->lowering.refCounts().retainValue(value, valueType);
  this->lowering.arrayLowerer().emitPush(values, {value}, valueType);
  this->lowering.builder_.CreateBr(nextBlock);

  this->lowering.functionFrame().switchTo(errBlock);
  llvm::Value *error = this->lowering.resultLowerer().resultHalf(result, element, true);
  this->lowering.refCounts().retainValue(error, errorType);
  if (all) {
    if (collects) {
      this->lowering.arrayLowerer().emitPush(errors, {error}, errorType);
    }
    this->lowering.builder_.CreateBr(nextBlock);
  } else {
    this->lowering.refCounts().releaseValue(values, Type::array(valueType));
    this->lowering.builder_.CreateStore(
        this->lowering.resultLowerer().makeResult(
            resultType, this->lowering.builder_.getFalse(), nullptr, error,
            this->lowering.resultLowerer().resultSite(result, element)),
        resultSlot);
    this->lowering.builder_.CreateBr(doneBlock);
  }

  this->lowering.functionFrame().switchTo(nextBlock);
  this->lowering.builder_.CreateStore(
      this->lowering.builder_.CreateAdd(i, this->lowering.builder_.getInt64(1)), indexSlot);
  this->lowering.builder_.CreateBr(headBlock);

  this->lowering.functionFrame().switchTo(endBlock);
  if (!collects) {
    this->lowering.builder_.CreateStore(
        this->lowering.resultLowerer().makeResult(resultType, this->lowering.builder_.getTrue(),
                                                  values, nullptr),
        resultSlot);
  } else {
    llvm::Value *failed = this->lowering.builder_.CreateICmpNE(
        this->lowering.arrayLowerer().arrayLength(errors), this->lowering.builder_.getInt64(0));
    llvm::Value *okResult = this->lowering.resultLowerer().makeResult(
        resultType, this->lowering.builder_.getTrue(), values, nullptr);
    llvm::Value *errResult = this->lowering.resultLowerer().makeResult(
        resultType, this->lowering.builder_.getFalse(), nullptr, errors,
        this->lowering.errSites().site(call->loc()));
    llvm::BasicBlock *failBlock = this->lowering.functionFrame().newBlock("combine.failed");
    llvm::BasicBlock *passBlock = this->lowering.functionFrame().newBlock("combine.passed");
    this->lowering.builder_.CreateCondBr(failed, failBlock, passBlock);
    this->lowering.functionFrame().switchTo(failBlock);
    this->lowering.refCounts().releaseValue(values, Type::array(valueType));
    this->lowering.builder_.CreateStore(errResult, resultSlot);
    this->lowering.builder_.CreateBr(doneBlock);
    this->lowering.functionFrame().switchTo(passBlock);
    this->lowering.refCounts().releaseValue(errors, resultType->errType());
    this->lowering.builder_.CreateStore(okResult, resultSlot);
  }
  this->lowering.functionFrame().emitBlock(doneBlock);
  this->lowering.refCounts().dropOperand(array);

  return this->lowering.builder_.CreateLoad(this->lowering.moduleBuilder().lower(resultType),
                                            resultSlot);
}

llvm::Value *ResultCombineLowerer::emitCombineTuple(CallExpr *call) {
  bool all = call->builtin() == Builtin::ResultCombineWithAllErrors;
  auto *list = cast<ArrayLitExpr>(call->args[0]);
  const Type *resultType = call->type;
  const Type *tupleType = resultType->okType();
  const Type *errorType = all && resultType->errType()->isArray() ? resultType->errType()->inner()
                                                                  : resultType->errType();
  std::vector<llvm::Value *> results;
  std::vector<const Type *> types;
  for (const ArrayLitExpr::Element &e : list->elements) {
    results.push_back(this->lowering.exprLowerer().emitExpr(e.expr));
    types.push_back(e.expr->type);
  }
  llvm::Value *resultSlot = this->lowering.functionFrame().createStackSlot(
      this->lowering.moduleBuilder().lower(resultType), "combined");
  llvm::BasicBlock *doneBlock = this->lowering.functionFrame().newBlock("combine.done");
  llvm::Value *errors = all && !errorType->isNever()
                            ? this->lowering.groupLowerer().newArray(
                                  Type::array(errorType), this->lowering.builder_.getInt64(0),
                                  this->lowering.builder_.getInt64(0), "errors")
                            : nullptr;

  for (size_t i = 0; i < results.size(); ++i) {
    if (types[i]->errType()->isNever()) {
      continue;
    }
    llvm::BasicBlock *errBlock = this->lowering.functionFrame().newBlock("combine.err");
    llvm::BasicBlock *nextBlock = this->lowering.functionFrame().newBlock("combine.next");
    this->lowering.builder_.CreateCondBr(
        this->lowering.resultLowerer().resultIsOk(results[i], types[i]), nextBlock, errBlock);
    this->lowering.functionFrame().switchTo(errBlock);
    llvm::Value *error = this->lowering.resultLowerer().convertHalf(
        this->lowering.resultLowerer().resultHalf(results[i], types[i], true), types[i]->errType(),
        errorType);
    if (errors) {
      this->lowering.arrayLowerer().emitPush(errors, {error}, errorType);
    } else {
      for (size_t j = 0; j < results.size(); ++j) {
        if (j != i) {
          this->lowering.refCounts().releaseValue(results[j], types[j]);
        }
      }
      this->lowering.builder_.CreateStore(
          this->lowering.resultLowerer().makeResult(
              resultType, this->lowering.builder_.getFalse(), nullptr, error,
              this->lowering.resultLowerer().resultSite(results[i], types[i])),
          resultSlot);
      this->lowering.builder_.CreateBr(doneBlock);
      this->lowering.functionFrame().switchTo(nextBlock);
      continue;
    }
    this->lowering.functionFrame().emitBlock(nextBlock);
  }

  auto makeTuple = [&] {
    const Lowering::ObjectLayout &layout = this->lowering.objectLayouts().objectLayout(tupleType);
    std::vector<unsigned> keys;
    for (unsigned i = 0; i < results.size(); ++i) {
      keys.push_back(i);
    }
    llvm::Value *tuple = this->lowering.objectLayouts().allocObject(tupleType, keys);
    for (unsigned i = 0; i < results.size(); ++i) {
      this->lowering.builder_.CreateStore(
          this->lowering.resultLowerer().resultHalf(results[i], types[i], false),
          this->lowering.builder_.CreateStructGEP(layout.type, tuple, layout.elements[i]));
    }
    this->lowering.groupLowerer().adoptFields(tuple, tupleType);
    this->lowering.builder_.CreateStore(
        this->lowering.resultLowerer().makeResult(resultType, this->lowering.builder_.getTrue(),
                                                  tuple, nullptr),
        resultSlot);
  };
  if (!errors) {
    makeTuple();
  } else {
    llvm::BasicBlock *failBlock = this->lowering.functionFrame().newBlock("combine.failed");
    llvm::BasicBlock *passBlock = this->lowering.functionFrame().newBlock("combine.passed");
    this->lowering.builder_.CreateCondBr(
        this->lowering.builder_.CreateICmpNE(this->lowering.arrayLowerer().arrayLength(errors),
                                             this->lowering.builder_.getInt64(0)),
        failBlock, passBlock);
    this->lowering.functionFrame().switchTo(failBlock);
    for (size_t i = 0; i < results.size(); ++i) {
      this->lowering.refCounts().releaseValue(
          this->lowering.resultLowerer().resultHalf(results[i], types[i], false),
          types[i]->okType());
    }
    this->lowering.builder_.CreateStore(this->lowering.resultLowerer().makeResult(
                                            resultType, this->lowering.builder_.getFalse(), nullptr,
                                            errors, this->lowering.errSites().site(call->loc())),
                                        resultSlot);
    this->lowering.builder_.CreateBr(doneBlock);
    this->lowering.functionFrame().switchTo(passBlock);
    this->lowering.refCounts().releaseValue(errors, resultType->errType());
    makeTuple();
  }
  this->lowering.functionFrame().emitBlock(doneBlock);

  return this->lowering.builder_.CreateLoad(this->lowering.moduleBuilder().lower(resultType),
                                            resultSlot);
}

}
