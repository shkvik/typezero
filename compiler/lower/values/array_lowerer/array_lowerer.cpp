#include "compiler/lower/values/array_lowerer/array_lowerer.h"
#include "compiler/library/decimal/decimal_lowerer/decimal_lowerer.h"
#include "compiler/lower/errors/may_throw/may_throw.h"
#include "compiler/lower/errors/throw_lowerer/throw_lowerer.h"
#include "compiler/lower/expr/assign_lowerer/assign_lowerer.h"
#include "compiler/lower/expr/binary_lowerer/binary_lowerer.h"
#include "compiler/lower/expr/expr_lowerer/expr_lowerer.h"
#include "compiler/lower/function_frame/function_frame.h"
#include "compiler/lower/iterators/iterator_lowerer/iterator_lowerer.h"
#include "compiler/lower/layouts/type_descriptors/type_descriptors.h"
#include "compiler/lower/memory/group_lowerer/group_lowerer.h"
#include "compiler/lower/memory/ref_counts/ref_counts.h"
#include "compiler/lower/memory/store_lowerer/store_lowerer.h"
#include "compiler/lower/module_builder/module_builder.h"
#include "compiler/lower/objects/object_lowerer/object_lowerer.h"
#include "compiler/lower/stmt/loop_lowerer/loop_lowerer.h"
#include "compiler/lower/stmt/stmt_lowerer/stmt_lowerer.h"
#include "compiler/lower/types/optional_lowerer/optional_lowerer.h"
#include "compiler/lower/values/string_lowerer/string_lowerer.h"
#include "compiler/memory/memory_plan/memory_plan.h"
#include "llvm/IR/MDBuilder.h"
#include "llvm/Support/ErrorHandling.h"

using llvm::BasicBlock;
using llvm::cast;
using llvm::dyn_cast;
using llvm::isa;
using llvm::Value;

namespace tz {

namespace {

enum ArrayField : unsigned { kRefCount, kElementType, kData, kLength, kCapacity, kAlloc };

}

ArrayLowerer::ArrayLowerer(Lowering &lowering) : lowering_(lowering) {}

llvm::Value *ArrayLowerer::arrayField(llvm::Value *array, unsigned field) {
  return lowering_.builder_.CreateStructGEP(lowering_.arrayTy_, array, field);
}

llvm::Value *ArrayLowerer::arrayLength(llvm::Value *array) {
  llvm::Value *length = lowering_.builder_.CreateLoad(lowering_.builder_.getInt32Ty(),
                                                      arrayField(array, kLength), "length");

  return lowering_.builder_.CreateZExt(length, lowering_.builder_.getInt64Ty());
}

llvm::Value *ArrayLowerer::arrayData(llvm::Value *array) {
  return lowering_.builder_.CreateLoad(lowering_.builder_.getPtrTy(), arrayField(array, kData),
                                       "data");
}

llvm::Value *ArrayLowerer::elementAddress(llvm::Value *data, const Type *element,
                                          llvm::Value *index) {
  return lowering_.builder_.CreateInBoundsGEP(lowering_.moduleBuilder().lower(element), data,
                                              index);
}

llvm::Value *ArrayLowerer::emitCheckedRead(llvm::Value *inRange, const Type *resultType,
                                           llvm::function_ref<llvm::Value *()> read) {
  llvm::BasicBlock *before = lowering_.builder_.GetInsertBlock();
  llvm::BasicBlock *readBlock = lowering_.functionFrame().newBlock("read");
  llvm::BasicBlock *endBlock = lowering_.functionFrame().newBlock("read.end");
  lowering_.builder_.CreateCondBr(inRange, readBlock, endBlock);

  lowering_.functionFrame().switchTo(readBlock);
  llvm::Value *value = read();
  if (value->getType() != lowering_.moduleBuilder().lower(resultType)) {
    value =
        lowering_.optionalLowerer().makeOptional(value, lowering_.builder_.getTrue(), resultType);
  }
  llvm::BasicBlock *readEnd = lowering_.builder_.GetInsertBlock();
  lowering_.builder_.CreateBr(endBlock);

  lowering_.functionFrame().switchTo(endBlock);
  llvm::PHINode *phi = lowering_.builder_.CreatePHI(lowering_.moduleBuilder().lower(resultType), 2);
  phi->addIncoming(lowering_.optionalLowerer().undefinedValue(resultType), before);
  phi->addIncoming(value, readEnd);

  return phi;
}

void ArrayLowerer::emitPush(llvm::Value *array, llvm::ArrayRef<llvm::Value *> values,
                            const Type *element) {
  llvm::Type *i32 = lowering_.builder_.getInt32Ty();
  llvm::Value *length = lowering_.builder_.CreateLoad(i32, arrayField(array, kLength), "length");
  llvm::Value *capacity =
      lowering_.builder_.CreateLoad(i32, arrayField(array, kCapacity), "capacity");
  llvm::Value *newLength = lowering_.builder_.CreateAdd(
      length, lowering_.builder_.getInt32(static_cast<uint32_t>(values.size())));
  llvm::BasicBlock *growBlock = lowering_.functionFrame().newBlock("push.grow");
  llvm::BasicBlock *storeBlock = lowering_.functionFrame().newBlock("push.store");
  lowering_.builder_.CreateCondBr(lowering_.builder_.CreateICmpUGT(newLength, capacity), growBlock,
                                  storeBlock,
                                  llvm::MDBuilder(lowering_.ctx_).createUnlikelyBranchWeights());
  lowering_.functionFrame().switchTo(growBlock);
  lowering_.builder_.CreateCall(
      lowering_.rt_.arrGrow,
      {array, lowering_.builder_.CreateZExt(newLength, lowering_.builder_.getInt64Ty())});
  lowering_.builder_.CreateBr(storeBlock);

  lowering_.functionFrame().switchTo(storeBlock);
  llvm::Value *data = arrayData(array);
  llvm::Value *at = lowering_.builder_.CreateZExt(length, lowering_.builder_.getInt64Ty());
  for (size_t i = 0; i < values.size(); ++i) {
    lowering_.builder_.CreateStore(
        values[i],
        elementAddress(data, element,
                       lowering_.builder_.CreateAdd(at, lowering_.builder_.getInt64(i))));
  }
  lowering_.builder_.CreateStore(newLength, arrayField(array, kLength));
  if (lowering_.memory().refMayBeMember(Type::array(element))) {
    for (llvm::Value *value : values) {
      lowering_.storeLowerer().groupStore(array, value, element);
    }
  }
}

llvm::Value *ArrayLowerer::emitArrayLiteral(ArrayLitExpr *lit) {
  if (lit->type->isTuple()) {
    return lowering_.objectLowerer().emitTupleLiteral(lit);
  }
  const Type *element = lit->type->inner();
  llvm::Constant *descriptor = lowering_.typeDescriptors().typeDescriptor(element);
  bool spreads =
      llvm::any_of(lit->elements, [](const ArrayLitExpr::Element &e) { return e.spread; });

  if (!spreads) {
    llvm::Value *count = lowering_.builder_.getInt64(lit->elements.size());
    bool member = lowering_.makesMember(lit, lit->type);
    llvm::Value *array =
        lowering_.groupLowerer().newArray(lit->type, count, count, member, "array");
    lowering_.groupLowerer().noteUnstored(array, lit);
    if (lit->elements.empty()) {
      return array;
    }
    std::vector<Expr *> exprs;
    for (const ArrayLitExpr::Element &e : lit->elements) {
      exprs.push_back(e.expr);
    }
    llvm::Value *held = array;
    llvm::Value *data = arrayData(array);
    for (size_t i = 0; i < lit->elements.size(); ++i) {
      llvm::Value *value = lowering_.exprLowerer().emitExpr(lit->elements[i].expr);
      lowering_.builder_.CreateStore(value,
                                     elementAddress(data, element, lowering_.builder_.getInt64(i)));
      if (member) {
        lowering_.storeLowerer().groupStore(array, value, element);
      }
    }
    return held;
  }

  size_t plain =
      llvm::count_if(lit->elements, [](const ArrayLitExpr::Element &e) { return !e.spread; });
  (void)descriptor;
  llvm::Value *array = lowering_.groupLowerer().newArray(
      lit->type, lowering_.builder_.getInt64(0), lowering_.builder_.getInt64(plain),
      lowering_.makesMember(lit, lit->type), "array");
  lowering_.groupLowerer().noteUnstored(array, lit);
  llvm::Value *held = array;
  for (const ArrayLitExpr::Element &e : lit->elements) {
    if (e.spread && (e.expr->type->isIterator() || e.expr->type->isTypedArray())) {
      lowering_.iteratorLowerer().emitIteratorSpread(array, e.expr, element);
      continue;
    }
    if (e.spread) {
      Lowering::Operand source = lowering_.refCounts().emitOperand(e.expr, true);
      if (e.expr->type->isString()) {
        lowering_.builder_.CreateCall(lowering_.rt_.arrAppendCodePoints,
                                      {array, lowering_.stringLowerer().stringObj(source.value),
                                       lowering_.stringLowerer().stringMeta(source.value)});
      } else if (e.expr->type->isTuple()) {
        lowering_.objectLowerer().emitTupleSpread(array, source.value, e.expr->type, element);
      } else {
        lowering_.builder_.CreateCall(lowering_.rt_.arrAppend, {array, source.value});
      }
      lowering_.refCounts().dropOperand(source);
    } else {
      emitPush(array, {lowering_.exprLowerer().emitExpr(e.expr)}, element);
    }
  }

  return held;
}

llvm::Value *
ArrayLowerer::emitArrayFold(llvm::Value *array, const Type *element, llvm::Value *init,
                            llvm::function_ref<llvm::Value *(llvm::Value *, llvm::Value *)> step) {
  llvm::BasicBlock *before = lowering_.builder_.GetInsertBlock();
  llvm::Value *length = arrayLength(array);
  llvm::BasicBlock *headBlock = lowering_.functionFrame().newBlock("fold");
  llvm::BasicBlock *bodyBlock = lowering_.functionFrame().newBlock("fold.body");
  llvm::BasicBlock *endBlock = lowering_.functionFrame().newBlock("fold.end");
  lowering_.builder_.CreateBr(headBlock);
  lowering_.functionFrame().switchTo(headBlock);
  llvm::PHINode *i = lowering_.builder_.CreatePHI(lowering_.builder_.getInt64Ty(), 2, "i");
  llvm::PHINode *acc = lowering_.builder_.CreatePHI(init->getType(), 2, "acc");
  i->addIncoming(lowering_.builder_.getInt64(0), before);
  acc->addIncoming(init, before);
  lowering_.builder_.CreateCondBr(lowering_.builder_.CreateICmpULT(i, length), bodyBlock, endBlock);
  lowering_.functionFrame().switchTo(bodyBlock);
  llvm::Value *value = lowering_.builder_.CreateLoad(lowering_.moduleBuilder().lower(element),
                                                     elementAddress(arrayData(array), element, i));
  llvm::Value *next = step(acc, value);
  i->addIncoming(lowering_.builder_.CreateAdd(i, lowering_.builder_.getInt64(1)),
                 lowering_.builder_.GetInsertBlock());
  acc->addIncoming(next, lowering_.builder_.GetInsertBlock());
  lowering_.builder_.CreateBr(headBlock);
  lowering_.functionFrame().switchTo(endBlock);

  return acc;
}

llvm::Value *ArrayLowerer::emitNewArray(NewExpr *expr) {
  llvm::Constant *descriptor = lowering_.typeDescriptors().typeDescriptor(expr->type->inner());
  if (expr->args.empty()) {
    return lowering_.groupLowerer().newArray(expr->type, lowering_.builder_.getInt64(0),
                                             lowering_.builder_.getInt64(0));
  }
  llvm::FunctionCallee create = lowering_.rt_.arrNewLength;
  if (lowering_.isCyclic(expr->type)) {
    create = lowering_.groupLowerer().groupFunction(
        "tz_arr_new_length_group", lowering_.builder_.getPtrTy(),
        {lowering_.builder_.getPtrTy(), lowering_.builder_.getInt64Ty()});
  }
  llvm::Value *array = lowering_.builder_.CreateCall(
      create, {descriptor, lowering_.exprLowerer().emitExpr(expr->args[0])}, "array");

  return array;
}

llvm::Value *ArrayLowerer::emitArrayIndex(IndexExpr *index) {
  const Type *element = index->object->type->inner();
  Lowering::Operand array =
      lowering_.refCounts().emitOperand(index->object, AssignLowerer::cannotAssign(index->index));
  llvm::Value *position = lowering_.exprLowerer().emitExpr(index->index);
  auto read = [&] {
    llvm::Value *value =
        lowering_.builder_.CreateLoad(lowering_.moduleBuilder().lower(element),
                                      elementAddress(arrayData(array.value), element, position));
    lowering_.refCounts().retainValue(value, element);

    return value;
  };
  llvm::Value *result;
  if (!index->mayBeOutOfRange) {
    result = read();
  } else {
    result = emitCheckedRead(lowering_.builder_.CreateICmpULT(position, arrayLength(array.value)),
                             index->type, read);
  }
  lowering_.refCounts().dropOperand(array);

  return result;
}

llvm::Value *ArrayLowerer::emitElementAssign(AssignExpr *assign, bool needResult) {
  auto *index = cast<IndexExpr>(assign->target);
  const Type *element = index->type;
  Lowering::Operand array = lowering_.refCounts().emitOperand(
      index->object,
      AssignLowerer::cannotAssign(index->index) && AssignLowerer::cannotAssign(assign->value));
  llvm::Value *position = lowering_.exprLowerer().emitExpr(index->index);

  llvm::Value *value;
  if (!assign->op) {
    value = lowering_.exprLowerer().emitExpr(assign->value);
  } else {
    llvm::Value *old =
        lowering_.builder_.CreateLoad(lowering_.moduleBuilder().lower(element),
                                      elementAddress(arrayData(array.value), element, position));
    if (element->isString()) {
      lowering_.refCounts().retainString(old);
      Lowering::Operand rhs = lowering_.stringLowerer().emitConcatPart(assign->value, false);
      value = lowering_.stringLowerer().concatOperands({{old, true, element}, rhs});
    } else if (element->isDecimal()) {
      lowering_.refCounts().retainValue(old, element);
      value = lowering_.decimalLowerer().emitDecimalCompound(*assign->op, old, assign->value);
      lowering_.refCounts().releaseValue(old, element);
    } else {
      value = lowering_.binaryLowerer().emitArithmetic(
          *assign->op, old, lowering_.exprLowerer().emitExpr(assign->value), element);
    }
  }

  auto replace = [&] {
    lowering_.storeField_ = "[i]";
    lowering_.storeLoc_ = assign->loc();
    lowering_.storeLowerer().storeIn(array.value, array.type,
                                     elementAddress(arrayData(array.value), element, position),
                                     value, element);
    lowering_.storeField_.clear();
    lowering_.storeLoc_ = llvm::SMLoc();
  };
  if (!index->mayAppend) {
    replace();
  } else {
    llvm::BasicBlock *replaceBlock = lowering_.functionFrame().newBlock("store.replace");
    llvm::BasicBlock *appendBlock = lowering_.functionFrame().newBlock("store.append");
    llvm::BasicBlock *endBlock = lowering_.functionFrame().newBlock("store.end");
    lowering_.builder_.CreateCondBr(
        lowering_.builder_.CreateICmpULT(position, arrayLength(array.value)), replaceBlock,
        appendBlock);
    lowering_.functionFrame().switchTo(replaceBlock);
    replace();
    lowering_.builder_.CreateBr(endBlock);
    lowering_.functionFrame().switchTo(appendBlock);
    emitPush(array.value, {value}, element);
    lowering_.builder_.CreateBr(endBlock);
    lowering_.functionFrame().switchTo(endBlock);
  }
  lowering_.refCounts().dropOperand(array);
  if (!needResult) {
    return nullptr;
  }
  lowering_.refCounts().retainValue(value, element);

  return value;
}

llvm::Value *ArrayLowerer::emitLengthAssign(AssignExpr *assign, bool needResult) {
  auto *member = cast<MemberExpr>(assign->target);
  Lowering::Operand array =
      lowering_.refCounts().emitOperand(member->object, AssignLowerer::cannotAssign(assign->value));
  llvm::Value *length = lowering_.exprLowerer().emitExpr(assign->value);
  lowering_.builder_.CreateCall(lowering_.rt_.arrSetLength, {array.value, length});
  lowering_.refCounts().dropOperand(array);

  return needResult ? length : nullptr;
}

void ArrayLowerer::emitArrayForOf(ForOfStmt *stmt, llvm::Value *iterableSlot,
                                  llvm::BasicBlock *condBlock, llvm::BasicBlock *bodyBlock,
                                  llvm::BasicBlock *endBlock) {
  const Type *iterableType = stmt->iterable->type;
  bool isString = iterableType->isString();
  bool borrow = stmt->borrowElements && !stmt->binding.sym->boxed();
  const Type *element = isString ? iterableType : iterableType->inner();
  llvm::Type *i64 = lowering_.builder_.getInt64Ty();
  llvm::Value *indexSlot = lowering_.functionFrame().createStackSlot(i64, "for.index");
  lowering_.builder_.CreateStore(lowering_.builder_.getInt64(0), indexSlot);

  lowering_.functionFrame().emitBlock(condBlock);
  lowering_.orderAt(stmt->loc());
  lowering_.orderAt(stmt->iterable->loc());
  llvm::Value *iterable =
      lowering_.builder_.CreateLoad(lowering_.moduleBuilder().lower(iterableType), iterableSlot);
  llvm::Value *index = lowering_.builder_.CreateLoad(i64, indexSlot, "index");
  llvm::Value *length =
      isString ? lowering_.stringLowerer().stringLength(iterable) : arrayLength(iterable);
  lowering_.builder_.CreateCondBr(lowering_.builder_.CreateICmpULT(index, length), bodyBlock,
                                  endBlock);

  lowering_.functionFrame().switchTo(bodyBlock);
  lowering_.functionFrame().pushScope();
  llvm::Value *value;
  if (isString) {
    llvm::Value *out = lowering_.functionFrame().createStackSlot(lowering_.strTy_, "code.point");
    llvm::Value *units = lowering_.builder_.CreateCall(
        lowering_.rt_.strCodePointAt, {out, lowering_.stringLowerer().stringObj(iterable),
                                       lowering_.stringLowerer().stringMeta(iterable), index});
    lowering_.builder_.CreateStore(
        lowering_.builder_.CreateAdd(index, lowering_.builder_.CreateSExt(units, i64)), indexSlot);
    value = lowering_.builder_.CreateLoad(lowering_.strTy_, out);
  } else {
    value = lowering_.builder_.CreateLoad(lowering_.moduleBuilder().lower(element),
                                          elementAddress(arrayData(iterable), element, index));
    if (!borrow) {
      lowering_.refCounts().retainValue(value, element);
    }
    lowering_.builder_.CreateStore(
        lowering_.builder_.CreateAdd(index, lowering_.builder_.getInt64(1)), indexSlot);
  }
  if (borrow) {
    llvm::Value *slot = lowering_.functionFrame().createStackSlot(
        lowering_.moduleBuilder().lower(element), stmt->binding.name);
    lowering_.addresses_[stmt->binding.sym] = slot;
    lowering_.builder_.CreateStore(value, slot);
  } else {
    lowering_.storeLowerer().storeVar(
        stmt->binding.sym,
        lowering_.functionFrame().createLocal(stmt->binding.sym, stmt->binding.name), value, true);
  }

  lowering_.loopLowerer().pushLoop(endBlock, condBlock, lowering_.scopes_.size() - 1);
  lowering_.stmtLowerer().emitStmt(stmt->body);
  lowering_.targets_.pop_back();
  lowering_.functionFrame().popScope();
  lowering_.functionFrame().branchTo(condBlock);
}

}
