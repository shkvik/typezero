#include "compiler/lower/values/array_lowerer/array_lowerer.h"
#include "compiler/lower/expr/assign_lowerer/assign_lowerer.h"
#include "compiler/lower/expr/binary_lowerer/binary_lowerer.h"
#include "compiler/lower/expr/expr_lowerer/expr_lowerer.h"
#include "compiler/lower/function_frame/function_frame.h"
#include "compiler/lower/iterators/iterator_lowerer/iterator_lowerer.h"
#include "compiler/lower/layouts/type_descriptors/type_descriptors.h"
#include "compiler/lower/library/decimal/decimal_lowerer/decimal_lowerer.h"
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

using llvm::cast;

namespace tz {

namespace {

enum ArrayField : unsigned { kRefCount, kElementType, kData, kLength, kCapacity, kAlloc };

}

ArrayLowerer::ArrayLowerer(Lowering &owner)
    : lowering(owner) {}

llvm::Value *ArrayLowerer::arrayField(llvm::Value *array, unsigned field) {
  return this->lowering.builder.CreateStructGEP(this->lowering.arrayTy, array, field);
}

llvm::Value *ArrayLowerer::arrayLength(llvm::Value *array) {
  llvm::Value *length = this->lowering.builder.CreateLoad(
      this->lowering.builder.getInt32Ty(), this->arrayField(array, kLength), "length");

  return this->lowering.builder.CreateZExt(length, this->lowering.builder.getInt64Ty());
}

llvm::Value *ArrayLowerer::arrayData(llvm::Value *array) {
  return this->lowering.builder.CreateLoad(this->lowering.builder.getPtrTy(),
                                           this->arrayField(array, kData), "data");
}

llvm::Value *ArrayLowerer::elementAddress(llvm::Value *data, const Type *element,
                                          llvm::Value *index) {
  return this->lowering.builder.CreateInBoundsGEP(this->lowering.moduleBuilder().lower(element),
                                                  data, index);
}

llvm::Value *ArrayLowerer::emitCheckedRead(llvm::Value *inRange, const Type *resultType,
                                           llvm::function_ref<llvm::Value *()> read) {
  llvm::BasicBlock *before = this->lowering.builder.GetInsertBlock();
  llvm::BasicBlock *readBlock = this->lowering.functionFrame().newBlock("read");
  llvm::BasicBlock *endBlock = this->lowering.functionFrame().newBlock("read.end");
  this->lowering.builder.CreateCondBr(inRange, readBlock, endBlock);

  this->lowering.functionFrame().switchTo(readBlock);
  llvm::Value *value = read();
  if (value->getType() != this->lowering.moduleBuilder().lower(resultType)) {
    value = this->lowering.optionalLowerer().makeOptional(value, this->lowering.builder.getTrue(),
                                                          resultType);
  }
  llvm::BasicBlock *readEnd = this->lowering.builder.GetInsertBlock();
  this->lowering.builder.CreateBr(endBlock);

  this->lowering.functionFrame().switchTo(endBlock);
  llvm::PHINode *phi =
      this->lowering.builder.CreatePHI(this->lowering.moduleBuilder().lower(resultType), 2);
  phi->addIncoming(this->lowering.optionalLowerer().undefinedValue(resultType), before);
  phi->addIncoming(value, readEnd);

  return phi;
}

void ArrayLowerer::emitPush(llvm::Value *array, llvm::ArrayRef<llvm::Value *> values,
                            const Type *element) {
  llvm::Type *i32 = this->lowering.builder.getInt32Ty();
  llvm::Value *length =
      this->lowering.builder.CreateLoad(i32, this->arrayField(array, kLength), "length");
  llvm::Value *capacity =
      this->lowering.builder.CreateLoad(i32, this->arrayField(array, kCapacity), "capacity");
  llvm::Value *newLength = this->lowering.builder.CreateAdd(
      length, this->lowering.builder.getInt32(static_cast<uint32_t>(values.size())));
  llvm::BasicBlock *growBlock = this->lowering.functionFrame().newBlock("push.grow");
  llvm::BasicBlock *storeBlock = this->lowering.functionFrame().newBlock("push.store");
  this->lowering.builder.CreateCondBr(
      this->lowering.builder.CreateICmpUGT(newLength, capacity), growBlock, storeBlock,
      llvm::MDBuilder(this->lowering.ctx).createUnlikelyBranchWeights());
  this->lowering.functionFrame().switchTo(growBlock);
  this->lowering.builder.CreateCall(
      this->lowering.rt_.arrGrow,
      {array, this->lowering.builder.CreateZExt(newLength, this->lowering.builder.getInt64Ty())});
  this->lowering.builder.CreateBr(storeBlock);

  this->lowering.functionFrame().switchTo(storeBlock);
  for (llvm::Value *value : values) {
    this->lowering.storeLowerer().storeBarrier(array, value, element);
  }
  llvm::Value *data = this->arrayData(array);
  llvm::Value *at = this->lowering.builder.CreateZExt(length, this->lowering.builder.getInt64Ty());
  for (size_t i = 0; i < values.size(); ++i) {
    this->lowering.builder.CreateStore(
        values[i], this->elementAddress(
                       data, element,
                       this->lowering.builder.CreateAdd(at, this->lowering.builder.getInt64(i))));
  }
  this->lowering.builder.CreateStore(newLength, this->arrayField(array, kLength));
  if (this->lowering.memory().refMayCarryMember(Type::array(element))) {
    for (llvm::Value *value : values) {
      this->lowering.storeLowerer().groupStore(array, value, element);
    }
  }
}

llvm::Value *ArrayLowerer::emitArrayLiteral(ArrayLitExpr *lit) {
  if (lit->type->isTuple()) {
    return this->lowering.objectLowerer().emitTupleLiteral(lit);
  }
  const Type *element = lit->type->inner();
  llvm::Constant *descriptor = this->lowering.typeDescriptors().typeDescriptor(element);
  bool spreads =
      llvm::any_of(lit->elements, [](const ArrayLitExpr::Element &e) { return e.spread; });

  if (!spreads) {
    llvm::Value *count = this->lowering.builder.getInt64(lit->elements.size());
    bool member = this->lowering.makesMember(lit, lit->type);
    llvm::Value *array =
        this->lowering.groupLowerer().newArray(lit->type, count, count, member, "array");
    this->lowering.groupLowerer().noteUnstored(array, lit);
    if (lit->elements.empty()) {
      return array;
    }
    std::vector<Expr *> exprs;
    for (const ArrayLitExpr::Element &e : lit->elements) {
      exprs.push_back(e.expr);
    }
    llvm::Value *data = this->arrayData(array);
    std::vector<llvm::Value *> values;
    for (size_t i = 0; i < lit->elements.size(); ++i) {
      llvm::Value *value = this->lowering.exprLowerer().emitExpr(lit->elements[i].expr);
      this->lowering.builder.CreateStore(
          value, this->elementAddress(data, element, this->lowering.builder.getInt64(i)));
      values.push_back(value);
    }
    if (member) {
      for (llvm::Value *value : values) {
        this->lowering.storeLowerer().groupStore(array, value, element);
      }
    }
    return array;
  }

  size_t plain =
      llvm::count_if(lit->elements, [](const ArrayLitExpr::Element &e) { return !e.spread; });
  (void)descriptor;
  llvm::Value *array = this->lowering.groupLowerer().newArray(
      lit->type, this->lowering.builder.getInt64(0), this->lowering.builder.getInt64(plain),
      this->lowering.makesMember(lit, lit->type), "array");
  this->lowering.groupLowerer().noteUnstored(array, lit);
  for (const ArrayLitExpr::Element &e : lit->elements) {
    if (e.spread && (e.expr->type->isIterator() || e.expr->type->isTypedArray())) {
      this->lowering.iteratorLowerer().emitIteratorSpread(array, e.expr, element);
      continue;
    }
    if (e.spread) {
      Lowering::Operand source = this->lowering.refCounts().emitOperand(e.expr, true);
      if (e.expr->type->isString()) {
        this->lowering.builder.CreateCall(
            this->lowering.rt_.arrAppendCodePoints,
            {array, this->lowering.stringLowerer().stringObj(source.value),
             this->lowering.stringLowerer().stringMeta(source.value)});
      } else if (e.expr->type->isTuple()) {
        this->lowering.objectLowerer().emitTupleSpread(array, source.value, e.expr->type, element);
      } else {
        this->lowering.builder.CreateCall(this->lowering.rt_.arrAppend, {array, source.value});
      }
      this->lowering.refCounts().dropOperand(source);
    } else {
      this->emitPush(array, {this->lowering.exprLowerer().emitExpr(e.expr)}, element);
    }
  }

  return array;
}

llvm::Value *
ArrayLowerer::emitArrayFold(llvm::Value *array, const Type *element, llvm::Value *init,
                            llvm::function_ref<llvm::Value *(llvm::Value *, llvm::Value *)> step) {
  llvm::BasicBlock *before = this->lowering.builder.GetInsertBlock();
  llvm::Value *length = this->arrayLength(array);
  llvm::BasicBlock *headBlock = this->lowering.functionFrame().newBlock("fold");
  llvm::BasicBlock *bodyBlock = this->lowering.functionFrame().newBlock("fold.body");
  llvm::BasicBlock *endBlock = this->lowering.functionFrame().newBlock("fold.end");
  this->lowering.builder.CreateBr(headBlock);
  this->lowering.functionFrame().switchTo(headBlock);
  llvm::PHINode *i = this->lowering.builder.CreatePHI(this->lowering.builder.getInt64Ty(), 2, "i");
  llvm::PHINode *acc = this->lowering.builder.CreatePHI(init->getType(), 2, "acc");
  i->addIncoming(this->lowering.builder.getInt64(0), before);
  acc->addIncoming(init, before);
  this->lowering.builder.CreateCondBr(this->lowering.builder.CreateICmpULT(i, length), bodyBlock,
                                      endBlock);
  this->lowering.functionFrame().switchTo(bodyBlock);
  llvm::Value *value =
      this->lowering.builder.CreateLoad(this->lowering.moduleBuilder().lower(element),
                                        this->elementAddress(this->arrayData(array), element, i));
  llvm::Value *next = step(acc, value);
  i->addIncoming(this->lowering.builder.CreateAdd(i, this->lowering.builder.getInt64(1)),
                 this->lowering.builder.GetInsertBlock());
  acc->addIncoming(next, this->lowering.builder.GetInsertBlock());
  this->lowering.builder.CreateBr(headBlock);
  this->lowering.functionFrame().switchTo(endBlock);

  return acc;
}

llvm::Value *ArrayLowerer::emitNewArray(NewExpr *expr) {
  llvm::Constant *descriptor = this->lowering.typeDescriptors().typeDescriptor(expr->type->inner());
  if (expr->args.empty()) {
    return this->lowering.groupLowerer().newArray(expr->type, this->lowering.builder.getInt64(0),
                                                  this->lowering.builder.getInt64(0));
  }
  llvm::FunctionCallee create = this->lowering.rt_.arrNewLength;
  if (this->lowering.isCyclic(expr->type)) {
    create = this->lowering.groupLowerer().groupFunction(
        "tz_arr_new_length_group", this->lowering.builder.getPtrTy(),
        {this->lowering.builder.getPtrTy(), this->lowering.builder.getInt64Ty()});
  }
  llvm::Value *array = this->lowering.builder.CreateCall(
      create, {descriptor, this->lowering.exprLowerer().emitExpr(expr->args[0])}, "array");

  return array;
}

llvm::Value *ArrayLowerer::emitArrayIndex(IndexExpr *index) {
  const Type *element = index->object->type->inner();
  Lowering::Operand array = this->lowering.refCounts().emitOperand(
      index->object, AssignLowerer::cannotAssign(index->index));
  llvm::Value *position = this->lowering.exprLowerer().emitExpr(index->index);
  auto read = [&] {
    llvm::Value *value = this->lowering.builder.CreateLoad(
        this->lowering.moduleBuilder().lower(element),
        this->elementAddress(this->arrayData(array.value), element, position));
    this->lowering.refCounts().retainValue(value, element);

    return value;
  };
  llvm::Value *result;
  if (!index->mayBeOutOfRange()) {
    result = read();
  } else {
    result = this->emitCheckedRead(
        this->lowering.builder.CreateICmpULT(position, this->arrayLength(array.value)), index->type,
        read);
  }
  this->lowering.refCounts().dropOperand(array);

  return result;
}

llvm::Value *ArrayLowerer::emitElementAssign(AssignExpr *assign, bool needResult) {
  auto *index = cast<IndexExpr>(assign->target);
  const Type *element = index->type;
  Lowering::Operand array = this->lowering.refCounts().emitOperand(
      index->object,
      AssignLowerer::cannotAssign(index->index) && AssignLowerer::cannotAssign(assign->value));
  llvm::Value *position = this->lowering.exprLowerer().emitExpr(index->index);

  llvm::Value *value;
  if (!assign->op) {
    value = this->lowering.exprLowerer().emitExpr(assign->value);
  } else {
    llvm::Value *old = this->lowering.builder.CreateLoad(
        this->lowering.moduleBuilder().lower(element),
        this->elementAddress(this->arrayData(array.value), element, position));
    if (element->isString()) {
      this->lowering.refCounts().retainString(old);
      Lowering::Operand rhs = this->lowering.stringLowerer().emitConcatPart(assign->value, false);
      value = this->lowering.stringLowerer().concatOperands({{old, true, element}, rhs});
    } else if (element->isDecimal()) {
      this->lowering.refCounts().retainValue(old, element);
      value = this->lowering.decimalLowerer().emitDecimalCompound(*assign->op, old, assign->value);
      this->lowering.refCounts().releaseValue(old, element);
    } else {
      value = this->lowering.binaryLowerer().emitArithmetic(
          *assign->op, old, this->lowering.exprLowerer().emitExpr(assign->value), element,
          assign->rangeProven);
    }
  }

  auto replace = [&] {
    this->lowering.storeField = "[i]";
    this->lowering.storeLoc = assign->loc();
    this->lowering.storeLowerer().storeIn(
        array.value, array.type,
        this->elementAddress(this->arrayData(array.value), element, position), value, element);
    this->lowering.storeField.clear();
    this->lowering.storeLoc = llvm::SMLoc();
  };
  if (!index->mayAppend()) {
    replace();
  } else {
    llvm::BasicBlock *replaceBlock = this->lowering.functionFrame().newBlock("store.replace");
    llvm::BasicBlock *appendBlock = this->lowering.functionFrame().newBlock("store.append");
    llvm::BasicBlock *endBlock = this->lowering.functionFrame().newBlock("store.end");
    this->lowering.builder.CreateCondBr(
        this->lowering.builder.CreateICmpULT(position, this->arrayLength(array.value)),
        replaceBlock, appendBlock);
    this->lowering.functionFrame().switchTo(replaceBlock);
    replace();
    this->lowering.builder.CreateBr(endBlock);
    this->lowering.functionFrame().switchTo(appendBlock);
    this->emitPush(array.value, {value}, element);
    this->lowering.builder.CreateBr(endBlock);
    this->lowering.functionFrame().switchTo(endBlock);
  }
  this->lowering.refCounts().dropOperand(array);
  if (!needResult) {
    return nullptr;
  }
  this->lowering.refCounts().retainValue(value, element);

  return value;
}

llvm::Value *ArrayLowerer::emitLengthAssign(AssignExpr *assign, bool needResult) {
  auto *member = cast<MemberExpr>(assign->target);
  Lowering::Operand array = this->lowering.refCounts().emitOperand(
      member->object, AssignLowerer::cannotAssign(assign->value));
  llvm::Value *length = this->lowering.exprLowerer().emitExpr(assign->value);
  this->lowering.builder.CreateCall(this->lowering.rt_.arrSetLength, {array.value, length});
  this->lowering.refCounts().dropOperand(array);

  return needResult ? length : nullptr;
}

void ArrayLowerer::emitArrayForOf(ForOfStmt *stmt, llvm::Value *iterableSlot,
                                  llvm::BasicBlock *condBlock, llvm::BasicBlock *bodyBlock,
                                  llvm::BasicBlock *endBlock) {
  const Type *iterableType = stmt->iterable->type;
  bool isString = iterableType->isString();
  bool borrow = stmt->borrowElements && !stmt->binding.sym->boxed();
  const Type *element = isString ? iterableType : iterableType->inner();
  llvm::Type *i64 = this->lowering.builder.getInt64Ty();
  llvm::Value *indexSlot = this->lowering.functionFrame().createStackSlot(i64, "for.index");
  this->lowering.builder.CreateStore(this->lowering.builder.getInt64(0), indexSlot);

  this->lowering.functionFrame().emitBlock(condBlock);
  this->lowering.orderAt(stmt->loc());
  this->lowering.orderAt(stmt->iterable->loc());
  llvm::Value *iterable = this->lowering.builder.CreateLoad(
      this->lowering.moduleBuilder().lower(iterableType), iterableSlot);
  llvm::Value *index = this->lowering.builder.CreateLoad(i64, indexSlot, "index");
  llvm::Value *length = isString ? this->lowering.stringLowerer().stringLength(iterable)
                                 : this->arrayLength(iterable);
  this->lowering.builder.CreateCondBr(this->lowering.builder.CreateICmpULT(index, length),
                                      bodyBlock, endBlock);

  this->lowering.functionFrame().switchTo(bodyBlock);
  this->lowering.functionFrame().pushScope();
  llvm::Value *value;
  if (isString) {
    llvm::Value *out =
        this->lowering.functionFrame().createStackSlot(this->lowering.strTy, "code.point");
    llvm::Value *units = this->lowering.builder.CreateCall(
        this->lowering.rt_.strCodePointAt,
        {out, this->lowering.stringLowerer().stringObj(iterable),
         this->lowering.stringLowerer().stringMeta(iterable), index});
    this->lowering.builder.CreateStore(
        this->lowering.builder.CreateAdd(index, this->lowering.builder.CreateSExt(units, i64)),
        indexSlot);
    value = this->lowering.builder.CreateLoad(this->lowering.strTy, out);
  } else {
    value = this->lowering.builder.CreateLoad(
        this->lowering.moduleBuilder().lower(element),
        this->elementAddress(this->arrayData(iterable), element, index));
    if (!borrow) {
      this->lowering.refCounts().retainValue(value, element);
    }
    this->lowering.builder.CreateStore(
        this->lowering.builder.CreateAdd(index, this->lowering.builder.getInt64(1)), indexSlot);
  }
  if (borrow) {
    llvm::Value *slot = this->lowering.functionFrame().createStackSlot(
        this->lowering.moduleBuilder().lower(element), stmt->binding.name);
    this->lowering.addresses[stmt->binding.sym] = slot;
    this->lowering.builder.CreateStore(value, slot);
  } else {
    this->lowering.storeLowerer().storeVar(
        stmt->binding.sym,
        this->lowering.functionFrame().createLocal(stmt->binding.sym, stmt->binding.name), value,
        true);
  }

  this->lowering.loopLowerer().pushLoop(endBlock, condBlock, this->lowering.scopes.size() - 1);
  this->lowering.stmtLowerer().emitStmt(stmt->body);
  this->lowering.targets.pop_back();
  this->lowering.functionFrame().popScope();
  this->lowering.functionFrame().branchTo(condBlock);
}

}
