#include "compiler/library/result/result_combine_lowerer/result_combine_lowerer.h"
#include "compiler/lower/module_builder/module_builder.h"
#include "compiler/lower/function_frame/function_frame.h"
#include "compiler/lower/memory/ref_counts/ref_counts.h"
#include "compiler/lower/expr/expr_lowerer/expr_lowerer.h"
#include "compiler/lower/values/array_lowerer/array_lowerer.h"
#include "compiler/lower/memory/group_lowerer/group_lowerer.h"
#include "compiler/lower/layouts/object_layouts/object_layouts.h"
#include "compiler/library/result/result_lowerer/result_lowerer.h"
#include "llvm/IR/MDBuilder.h"
#include <functional>

using llvm::cast;
using llvm::dyn_cast;
using llvm::isa;
using llvm::BasicBlock;
using llvm::Value;

namespace tz {

ResultCombineLowerer::ResultCombineLowerer(CodeGen &codegen) : codegen_(codegen) {}

// Result.combine: the values of the Results in an array, or the first Err;
// combineWithAllErrors: the values, or every error.
llvm::Value *ResultCombineLowerer::emitCombine(CallExpr *call) {
  if (call->args[0]->type->isTuple()) {
    return emitCombineTuple(call);
  }
  bool all = call->builtin == Builtin::ResultCombineWithAllErrors;
  const Type *arrayType = call->args[0]->type, *element = arrayType->inner();
  const Type *valueType = element->okType(), *errorType = element->errType();
  const Type *resultType = call->type;
  llvm::Type *i64 = codegen_.builder_.getInt64Ty();

  CodeGen::Operand array = codegen_.refCounts().emitOperand(call->args[0], /*canBorrow=*/true);
  llvm::Value *length = codegen_.arrayLowerer().arrayLength(array.value);
  llvm::Value *values = codegen_.groupLowerer().newArray(Type::array(valueType), codegen_.builder_.getInt64(0), length, "values");
  bool collects = all && !errorType->isNever();
  llvm::Value *errors = collects ? codegen_.groupLowerer().newArray(Type::array(errorType), codegen_.builder_.getInt64(0),
                                            codegen_.builder_.getInt64(0), "errors")
                                 : nullptr;
  llvm::Value *resultSlot = codegen_.functionFrame().createStackSlot(codegen_.moduleBuilder().lower(resultType), "combined");

  llvm::Value *indexSlot = codegen_.functionFrame().createStackSlot(i64, "index");
  codegen_.builder_.CreateStore(codegen_.builder_.getInt64(0), indexSlot);
  llvm::BasicBlock *headBlock = codegen_.functionFrame().newBlock("combine");
  llvm::BasicBlock *bodyBlock = codegen_.functionFrame().newBlock("combine.body");
  llvm::BasicBlock *okBlock = codegen_.functionFrame().newBlock("combine.ok");
  llvm::BasicBlock *errBlock = codegen_.functionFrame().newBlock("combine.err");
  llvm::BasicBlock *nextBlock = codegen_.functionFrame().newBlock("combine.next");
  llvm::BasicBlock *endBlock = codegen_.functionFrame().newBlock("combine.end");
  llvm::BasicBlock *doneBlock = codegen_.functionFrame().newBlock("combine.done");
  codegen_.functionFrame().emitBlock(headBlock);
  llvm::Value *i = codegen_.builder_.CreateLoad(i64, indexSlot, "i");
  codegen_.builder_.CreateCondBr(codegen_.builder_.CreateICmpULT(i, length), bodyBlock, endBlock);

  codegen_.functionFrame().switchTo(bodyBlock);
  llvm::Value *result = codegen_.builder_.CreateLoad(
      codegen_.moduleBuilder().lower(element), codegen_.arrayLowerer().elementAddress(codegen_.arrayLowerer().arrayData(array.value), element, i), "result");
  codegen_.builder_.CreateCondBr(codegen_.resultLowerer().resultIsOk(result, element), okBlock, errBlock);

  // The array keeps its Results: what goes into the new arrays is counted
  // again.
  codegen_.functionFrame().switchTo(okBlock);
  llvm::Value *value = codegen_.resultLowerer().resultHalf(result, element, /*error=*/false);
  codegen_.refCounts().retainValue(value, valueType);
  codegen_.arrayLowerer().emitPush(values, {value}, valueType);
  codegen_.builder_.CreateBr(nextBlock);

  codegen_.functionFrame().switchTo(errBlock);
  llvm::Value *error = codegen_.resultLowerer().resultHalf(result, element, /*error=*/true);
  codegen_.refCounts().retainValue(error, errorType);
  if (all) {
    if (collects) {
      codegen_.arrayLowerer().emitPush(errors, {error}, errorType);
    }
    codegen_.builder_.CreateBr(nextBlock);
  } else {
    // The first Err is the result.
    codegen_.refCounts().releaseValue(values, Type::array(valueType));
    codegen_.builder_.CreateStore(codegen_.resultLowerer().makeResult(resultType, codegen_.builder_.getFalse(), nullptr, error), resultSlot);
    codegen_.builder_.CreateBr(doneBlock);
  }

  codegen_.functionFrame().switchTo(nextBlock);
  codegen_.builder_.CreateStore(codegen_.builder_.CreateAdd(i, codegen_.builder_.getInt64(1)), indexSlot);
  codegen_.builder_.CreateBr(headBlock);

  codegen_.functionFrame().switchTo(endBlock);
  if (!collects) {
    codegen_.builder_.CreateStore(codegen_.resultLowerer().makeResult(resultType, codegen_.builder_.getTrue(), values, nullptr), resultSlot);
  } else {
    // Any error makes the result an Err with all of them.
    llvm::Value *failed = codegen_.builder_.CreateICmpNE(codegen_.arrayLowerer().arrayLength(errors), codegen_.builder_.getInt64(0));
    llvm::Value *okResult = codegen_.resultLowerer().makeResult(resultType, codegen_.builder_.getTrue(), values, nullptr);
    llvm::Value *errResult = codegen_.resultLowerer().makeResult(resultType, codegen_.builder_.getFalse(), nullptr, errors);
    llvm::BasicBlock *failBlock = codegen_.functionFrame().newBlock("combine.failed");
    llvm::BasicBlock *passBlock = codegen_.functionFrame().newBlock("combine.passed");
    codegen_.builder_.CreateCondBr(failed, failBlock, passBlock);
    codegen_.functionFrame().switchTo(failBlock);
    codegen_.refCounts().releaseValue(values, Type::array(valueType));
    codegen_.builder_.CreateStore(errResult, resultSlot);
    codegen_.builder_.CreateBr(doneBlock);
    codegen_.functionFrame().switchTo(passBlock);
    codegen_.refCounts().releaseValue(errors, resultType->errType());
    codegen_.builder_.CreateStore(okResult, resultSlot);
  }
  codegen_.functionFrame().emitBlock(doneBlock);
  codegen_.refCounts().dropOperand(array);

  return codegen_.builder_.CreateLoad(codegen_.moduleBuilder().lower(resultType), resultSlot);
}

// Result.combine([a, b]) of Results with values of different types: their
// values make a tuple. The Results are evaluated first, as the elements of
// the list are in JavaScript, and tested in order; no list is made.
llvm::Value *ResultCombineLowerer::emitCombineTuple(CallExpr *call) {
  bool all = call->builtin == Builtin::ResultCombineWithAllErrors;
  auto *list = cast<ArrayLitExpr>(call->args[0]);
  const Type *resultType = call->type, *tupleType = resultType->okType();
  const Type *errorType = all && resultType->errType()->isArray() ? resultType->errType()->inner()
                                                                  : resultType->errType();
  std::vector<llvm::Value *> results;
  std::vector<const Type *> types;
  for (const ArrayLitExpr::Element &e : list->elements) {
    results.push_back(codegen_.exprLowerer().emitExpr(e.expr));
    types.push_back(e.expr->type);
  }
  llvm::Value *resultSlot = codegen_.functionFrame().createStackSlot(codegen_.moduleBuilder().lower(resultType), "combined");
  llvm::BasicBlock *doneBlock = codegen_.functionFrame().newBlock("combine.done");
  llvm::Value *errors =
      all && !errorType->isNever()
          ? codegen_.groupLowerer().newArray(Type::array(errorType), codegen_.builder_.getInt64(0), codegen_.builder_.getInt64(0), "errors")
          : nullptr;

  for (size_t i = 0; i < results.size(); ++i) {
    if (types[i]->errType()->isNever()) {
      continue; // always an Ok
    }
    llvm::BasicBlock *errBlock = codegen_.functionFrame().newBlock("combine.err");
    llvm::BasicBlock *nextBlock = codegen_.functionFrame().newBlock("combine.next");
    codegen_.builder_.CreateCondBr(codegen_.resultLowerer().resultIsOk(results[i], types[i]), nextBlock, errBlock);
    codegen_.functionFrame().switchTo(errBlock);
    llvm::Value *error = codegen_.resultLowerer().convertHalf(codegen_.resultLowerer().resultHalf(results[i], types[i], /*error=*/true),
                                     types[i]->errType(), errorType);
    if (errors) {
      codegen_.arrayLowerer().emitPush(errors, {error}, errorType);
    } else {
      // The first Err is the result; the other Results are dropped.
      for (size_t j = 0; j < results.size(); ++j) {
        if (j != i) {
          codegen_.refCounts().releaseValue(results[j], types[j]);
        }
      }
      codegen_.builder_.CreateStore(codegen_.resultLowerer().makeResult(resultType, codegen_.builder_.getFalse(), nullptr, error), resultSlot);
      codegen_.builder_.CreateBr(doneBlock);
      codegen_.functionFrame().switchTo(nextBlock);
      continue;
    }
    codegen_.functionFrame().emitBlock(nextBlock);
  }

  // The values go into a new tuple, unless some errors were collected.
  auto makeTuple = [&] {
    const CodeGen::ObjectLayout &layout = codegen_.objectLayouts().objectLayout(tupleType);
    std::vector<unsigned> keys;
    for (unsigned i = 0; i < results.size(); ++i) {
      keys.push_back(i);
    }
    llvm::Value *tuple = codegen_.objectLayouts().allocObject(tupleType, keys);
    for (unsigned i = 0; i < results.size(); ++i) {
      codegen_.builder_.CreateStore(codegen_.resultLowerer().resultHalf(results[i], types[i], /*error=*/false),
                           codegen_.builder_.CreateStructGEP(layout.type, tuple, layout.elements[i]));
    }
    codegen_.groupLowerer().adoptFields(tuple, tupleType);
    codegen_.builder_.CreateStore(codegen_.resultLowerer().makeResult(resultType, codegen_.builder_.getTrue(), tuple, nullptr), resultSlot);
  };
  if (!errors) {
    makeTuple();
  } else {
    llvm::BasicBlock *failBlock = codegen_.functionFrame().newBlock("combine.failed");
    llvm::BasicBlock *passBlock = codegen_.functionFrame().newBlock("combine.passed");
    codegen_.builder_.CreateCondBr(codegen_.builder_.CreateICmpNE(codegen_.arrayLowerer().arrayLength(errors), codegen_.builder_.getInt64(0)),
                          failBlock, passBlock);
    codegen_.functionFrame().switchTo(failBlock);
    for (size_t i = 0; i < results.size(); ++i) {
      codegen_.refCounts().releaseValue(codegen_.resultLowerer().resultHalf(results[i], types[i], /*error=*/false), types[i]->okType());
    }
    codegen_.builder_.CreateStore(codegen_.resultLowerer().makeResult(resultType, codegen_.builder_.getFalse(), nullptr, errors), resultSlot);
    codegen_.builder_.CreateBr(doneBlock);
    codegen_.functionFrame().switchTo(passBlock);
    codegen_.refCounts().releaseValue(errors, resultType->errType());
    makeTuple();
  }
  codegen_.functionFrame().emitBlock(doneBlock);

  return codegen_.builder_.CreateLoad(codegen_.moduleBuilder().lower(resultType), resultSlot);
}

} // namespace tz
