#include "compiler/lower/values/array_lowerer/array_lowerer.h"
#include "compiler/lower/module_builder/module_builder.h"
#include "compiler/lower/function_frame/function_frame.h"
#include "compiler/lower/memory/ref_counts/ref_counts.h"
#include "compiler/lower/types/optional_lowerer/optional_lowerer.h"
#include "compiler/lower/stmt/stmt_lowerer/stmt_lowerer.h"
#include "compiler/lower/stmt/loop_lowerer/loop_lowerer.h"
#include "compiler/lower/expr/expr_lowerer/expr_lowerer.h"
#include "compiler/lower/expr/binary_lowerer/binary_lowerer.h"
#include "compiler/lower/expr/assign_lowerer/assign_lowerer.h"
#include "compiler/lower/values/string_lowerer/string_lowerer.h"
#include "compiler/lower/layouts/type_descriptors/type_descriptors.h"
#include "compiler/lower/memory/memory_analysis/memory_analysis.h"
#include "compiler/lower/memory/group_lowerer/group_lowerer.h"
#include "compiler/lower/memory/store_lowerer/store_lowerer.h"
#include "compiler/lower/iterators/iterator_lowerer/iterator_lowerer.h"
#include "compiler/lower/objects/object_lowerer/object_lowerer.h"
#include "compiler/lower/errors/may_throw/may_throw.h"
#include "compiler/lower/errors/throw_lowerer/throw_lowerer.h"
#include "compiler/library/decimal/decimal_lowerer/decimal_lowerer.h"
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

ArrayLowerer::ArrayLowerer(CodeGen &codegen) : codegen_(codegen) {}

llvm::Value *ArrayLowerer::arrayField(llvm::Value *array, unsigned field) {
  return codegen_.builder_.CreateStructGEP(codegen_.arrayTy_, array, field);
}

llvm::Value *ArrayLowerer::arrayLength(llvm::Value *array) {
  llvm::Value *length =
      codegen_.builder_.CreateLoad(codegen_.builder_.getInt32Ty(), arrayField(array, kLength), "length");

  return codegen_.builder_.CreateZExt(length, codegen_.builder_.getInt64Ty());
}

llvm::Value *ArrayLowerer::arrayData(llvm::Value *array) {
  return codegen_.builder_.CreateLoad(codegen_.builder_.getPtrTy(), arrayField(array, kData), "data");
}

llvm::Value *ArrayLowerer::elementAddress(llvm::Value *data, const Type *element, llvm::Value *index) {
  return codegen_.builder_.CreateInBoundsGEP(codegen_.moduleBuilder().lower(element), data, index);
}

llvm::Value *ArrayLowerer::emitCheckedRead(llvm::Value *inRange, const Type *resultType,
                                      llvm::function_ref<llvm::Value *()> read) {
  llvm::BasicBlock *before = codegen_.builder_.GetInsertBlock();
  llvm::BasicBlock *readBlock = codegen_.functionFrame().newBlock("read");
  llvm::BasicBlock *endBlock = codegen_.functionFrame().newBlock("read.end");
  codegen_.builder_.CreateCondBr(inRange, readBlock, endBlock);

  codegen_.functionFrame().switchTo(readBlock);
  // An element that is itself `T | undefined` is the result as it is.
  llvm::Value *value = read();
  if (value->getType() != codegen_.moduleBuilder().lower(resultType)) {
    value = codegen_.optionalLowerer().makeOptional(value, codegen_.builder_.getTrue(), resultType);
  }
  llvm::BasicBlock *readEnd = codegen_.builder_.GetInsertBlock();
  codegen_.builder_.CreateBr(endBlock);

  codegen_.functionFrame().switchTo(endBlock);
  llvm::PHINode *phi = codegen_.builder_.CreatePHI(codegen_.moduleBuilder().lower(resultType), 2);
  phi->addIncoming(codegen_.optionalLowerer().undefinedValue(resultType), before);
  phi->addIncoming(value, readEnd);

  return phi;
}

// Appends owned values; the array takes them over.
void ArrayLowerer::emitPush(llvm::Value *array, llvm::ArrayRef<llvm::Value *> values,
                       const Type *element) {
  llvm::Type *i32 = codegen_.builder_.getInt32Ty();
  llvm::Value *length = codegen_.builder_.CreateLoad(i32, arrayField(array, kLength), "length");
  llvm::Value *capacity = codegen_.builder_.CreateLoad(i32, arrayField(array, kCapacity), "capacity");
  llvm::Value *newLength =
      codegen_.builder_.CreateAdd(length, codegen_.builder_.getInt32(static_cast<uint32_t>(values.size())));
  llvm::BasicBlock *growBlock = codegen_.functionFrame().newBlock("push.grow");
  llvm::BasicBlock *storeBlock = codegen_.functionFrame().newBlock("push.store");
  codegen_.builder_.CreateCondBr(codegen_.builder_.CreateICmpUGT(newLength, capacity), growBlock, storeBlock,
                        llvm::MDBuilder(codegen_.ctx_).createUnlikelyBranchWeights());
  codegen_.functionFrame().switchTo(growBlock);
  codegen_.builder_.CreateCall(codegen_.rt_.arrGrow, {array, codegen_.builder_.CreateZExt(newLength, codegen_.builder_.getInt64Ty())});
  codegen_.builder_.CreateBr(storeBlock);

  codegen_.functionFrame().switchTo(storeBlock);
  llvm::Value *data = arrayData(array);
  llvm::Value *at = codegen_.builder_.CreateZExt(length, codegen_.builder_.getInt64Ty());
  for (size_t i = 0; i < values.size(); ++i) {
    codegen_.builder_.CreateStore(
        values[i], elementAddress(data, element, codegen_.builder_.CreateAdd(at, codegen_.builder_.getInt64(i))));
  }
  codegen_.builder_.CreateStore(newLength, arrayField(array, kLength));
  // An array that may be a member of a group: its elements join it.
  if (codegen_.memoryAnalysis().refMayBeMember(Type::array(element))) {
    for (llvm::Value *value : values) {
      codegen_.storeLowerer().groupStore(array, value, element);
    }
  }
}

llvm::Value *ArrayLowerer::emitArrayLiteral(ArrayLitExpr *lit) {
  if (lit->type->isTuple()) {
    return codegen_.objectLowerer().emitTupleLiteral(lit);
  }
  const Type *element = lit->type->inner();
  llvm::Constant *descriptor = codegen_.typeDescriptors().typeDescriptor(element);
  bool spreads =
      llvm::any_of(lit->elements, [](const ArrayLitExpr::Element &e) { return e.spread; });

  if (!spreads) {
    llvm::Value *count = codegen_.builder_.getInt64(lit->elements.size());
    bool member = codegen_.makesMember(lit, lit->type);
    llvm::Value *array = codegen_.groupLowerer().newArray(lit->type, count, count, member, "array");
    codegen_.groupLowerer().noteUnstored(array, lit);
    if (lit->elements.empty()) {
      return array;
    }
    // Nothing can reach the new array while its elements are evaluated, so
    // its data pointer stays put. If one of them throws, the array goes
    // with those stored (the runtime made the rest zero).
    std::vector<Expr *> exprs;
    for (const ArrayLitExpr::Element &e : lit->elements) {
      exprs.push_back(e.expr);
    }
    CodeGen::Held held = codegen_.throwLowerer().hold(array, lit->type, llvm::any_of(exprs, [&](Expr *e) { return codegen_.mayThrow().exprThrows(e); }));
    llvm::Value *data = arrayData(array);
    for (size_t i = 0; i < lit->elements.size(); ++i) {
      llvm::Value *value = codegen_.exprLowerer().emitExpr(lit->elements[i].expr);
      codegen_.builder_.CreateStore(value, elementAddress(data, element, codegen_.builder_.getInt64(i)));
      if (member) {
        codegen_.storeLowerer().groupStore(array, value, element);
      }
    }
    return codegen_.throwLowerer().take(held);
  }

  size_t plain =
      llvm::count_if(lit->elements, [](const ArrayLitExpr::Element &e) { return !e.spread; });
  (void)descriptor;
  llvm::Value *array = codegen_.groupLowerer().newArray(lit->type, codegen_.builder_.getInt64(0), codegen_.builder_.getInt64(plain),
                                codegen_.makesMember(lit, lit->type), "array");
  codegen_.groupLowerer().noteUnstored(array, lit);
  CodeGen::Held held =
      codegen_.throwLowerer().hold(array, lit->type, llvm::any_of(lit->elements, [&](const ArrayLitExpr::Element &e) {
             return codegen_.mayThrow().exprThrows(e.expr);
           }));
  for (const ArrayLitExpr::Element &e : lit->elements) {
    if (e.spread && (e.expr->type->isIterator() || e.expr->type->isTypedArray())) {
      codegen_.iteratorLowerer().emitIteratorSpread(array, e.expr, element);
      continue;
    }
    if (e.spread) {
      // Appended right away, so a variable can be borrowed. A string adds
      // its characters.
      CodeGen::Operand source = codegen_.refCounts().emitOperand(e.expr, /*canBorrow=*/true);
      if (e.expr->type->isString()) {
        codegen_.builder_.CreateCall(codegen_.rt_.arrAppendCodePoints,
                            {array, codegen_.stringLowerer().stringObj(source.value), codegen_.stringLowerer().stringMeta(source.value)});
      } else if (e.expr->type->isTuple()) {
        codegen_.objectLowerer().emitTupleSpread(array, source.value, e.expr->type, element);
      } else {
        codegen_.builder_.CreateCall(codegen_.rt_.arrAppend, {array, source.value});
      }
      codegen_.refCounts().dropOperand(source);
    } else {
      emitPush(array, {codegen_.exprLowerer().emitExpr(e.expr)}, element);
    }
  }

  return codegen_.throwLowerer().take(held);
}

llvm::Value *
ArrayLowerer::emitArrayFold(llvm::Value *array, const Type *element, llvm::Value *init,
                       llvm::function_ref<llvm::Value *(llvm::Value *, llvm::Value *)> step) {
  llvm::BasicBlock *before = codegen_.builder_.GetInsertBlock();
  llvm::Value *length = arrayLength(array);
  llvm::BasicBlock *headBlock = codegen_.functionFrame().newBlock("fold");
  llvm::BasicBlock *bodyBlock = codegen_.functionFrame().newBlock("fold.body");
  llvm::BasicBlock *endBlock = codegen_.functionFrame().newBlock("fold.end");
  codegen_.builder_.CreateBr(headBlock);
  codegen_.functionFrame().switchTo(headBlock);
  llvm::PHINode *i = codegen_.builder_.CreatePHI(codegen_.builder_.getInt64Ty(), 2, "i");
  llvm::PHINode *acc = codegen_.builder_.CreatePHI(init->getType(), 2, "acc");
  i->addIncoming(codegen_.builder_.getInt64(0), before);
  acc->addIncoming(init, before);
  codegen_.builder_.CreateCondBr(codegen_.builder_.CreateICmpULT(i, length), bodyBlock, endBlock);
  codegen_.functionFrame().switchTo(bodyBlock);
  llvm::Value *value =
      codegen_.builder_.CreateLoad(codegen_.moduleBuilder().lower(element), elementAddress(arrayData(array), element, i));
  llvm::Value *next = step(acc, value);
  i->addIncoming(codegen_.builder_.CreateAdd(i, codegen_.builder_.getInt64(1)), codegen_.builder_.GetInsertBlock());
  acc->addIncoming(next, codegen_.builder_.GetInsertBlock());
  codegen_.builder_.CreateBr(headBlock);
  codegen_.functionFrame().switchTo(endBlock);

  return acc;
}

llvm::Value *ArrayLowerer::emitNewArray(NewExpr *expr) {
  llvm::Constant *descriptor = codegen_.typeDescriptors().typeDescriptor(expr->type->inner());
  if (expr->args.empty()) {
    return codegen_.groupLowerer().newArray(expr->type, codegen_.builder_.getInt64(0), codegen_.builder_.getInt64(0));
  }
  // The runtime throws RangeError for a negative length, as JavaScript does.
  llvm::FunctionCallee create = codegen_.rt_.arrNewLength;
  if (codegen_.isCyclic(expr->type)) {
    create = codegen_.groupLowerer().groupFunction("tz_arr_new_length_group", codegen_.builder_.getPtrTy(),
                           {codegen_.builder_.getPtrTy(), codegen_.builder_.getInt64Ty()});
  }
  llvm::Value *array = codegen_.builder_.CreateCall(create, {descriptor, codegen_.exprLowerer().emitExpr(expr->args[0])}, "array");
  codegen_.emitThrowCheckIf(codegen_.mayThrow().exprThrows(expr));

  return array;
}

llvm::Value *ArrayLowerer::emitArrayIndex(IndexExpr *index) {
  const Type *element = index->object->type->inner();
  CodeGen::Operand array = codegen_.refCounts().emitOperand(index->object, AssignLowerer::cannotAssign(index->index));
  codegen_.throwLowerer().holdOperand(array, codegen_.mayThrow().exprThrows(index->index));
  llvm::Value *position = codegen_.exprLowerer().emitExpr(index->index);
  codegen_.throwLowerer().takeOperand(array);
  auto read = [&] {
    llvm::Value *value = codegen_.builder_.CreateLoad(
        codegen_.moduleBuilder().lower(element), elementAddress(arrayData(array.value), element, position));
    codegen_.refCounts().retainValue(value, element);

    return value;
  };
  llvm::Value *result;
  if (!index->mayBeOutOfRange) {
    result = read();
  } else {
    result = emitCheckedRead(codegen_.builder_.CreateICmpULT(position, arrayLength(array.value)),
                             index->type, read);
  }
  codegen_.refCounts().dropOperand(array);

  return result;
}

// `a[i] = v` and `a[i] op= v`. The store happens after v is evaluated, which
// may have changed the array, so the data pointer is loaded only then.
llvm::Value *ArrayLowerer::emitElementAssign(AssignExpr *assign, bool needResult) {
  auto *index = cast<IndexExpr>(assign->target);
  const Type *element = index->type;
  CodeGen::Operand array =
      codegen_.refCounts().emitOperand(index->object, AssignLowerer::cannotAssign(index->index) && AssignLowerer::cannotAssign(assign->value));
  codegen_.throwLowerer().holdOperand(array, codegen_.mayThrow().exprThrows(index->index) || codegen_.mayThrow().exprThrows(assign->value));
  llvm::Value *position = codegen_.exprLowerer().emitExpr(index->index);

  llvm::Value *value;
  if (!assign->op) {
    value = codegen_.exprLowerer().emitExpr(assign->value);
  } else {
    // As in JavaScript, the old value is read before the right side runs.
    llvm::Value *old = codegen_.builder_.CreateLoad(
        codegen_.moduleBuilder().lower(element), elementAddress(arrayData(array.value), element, position));
    if (element->isString()) {
      codegen_.refCounts().retainString(old); // the right side may replace the element
      CodeGen::Operand rhs = codegen_.stringLowerer().emitConcatPart(assign->value, /*canBorrow=*/false);
      value = codegen_.stringLowerer().concatOperands({{old, true, element}, rhs});
    } else if (element->isDecimal()) {
      codegen_.refCounts().retainValue(old, element); // the right side may replace the element
      value = codegen_.decimalLowerer().emitDecimalCompound(*assign->op, old, assign->value);
      codegen_.refCounts().releaseValue(old, element);
    } else {
      value = codegen_.binaryLowerer().emitArithmetic(*assign->op, old, codegen_.exprLowerer().emitExpr(assign->value), element);
    }
  }

  codegen_.throwLowerer().takeOperand(array);
  auto replace = [&] {
    codegen_.storeField_ = "[i]";
    codegen_.storeLoc_ = assign->loc();
    codegen_.storeLowerer().storeIn(array.value, array.type, elementAddress(arrayData(array.value), element, position),
            value, element);
    codegen_.storeField_.clear();
    codegen_.storeLoc_ = llvm::SMLoc();
  };
  if (!index->mayAppend) {
    replace();
  } else {
    // Sema proved index <= length: either a replacement or an append.
    llvm::BasicBlock *replaceBlock = codegen_.functionFrame().newBlock("store.replace");
    llvm::BasicBlock *appendBlock = codegen_.functionFrame().newBlock("store.append");
    llvm::BasicBlock *endBlock = codegen_.functionFrame().newBlock("store.end");
    codegen_.builder_.CreateCondBr(codegen_.builder_.CreateICmpULT(position, arrayLength(array.value)), replaceBlock,
                          appendBlock);
    codegen_.functionFrame().switchTo(replaceBlock);
    replace();
    codegen_.builder_.CreateBr(endBlock);
    codegen_.functionFrame().switchTo(appendBlock);
    emitPush(array.value, {value}, element);
    codegen_.builder_.CreateBr(endBlock);
    codegen_.functionFrame().switchTo(endBlock);
  }
  codegen_.refCounts().dropOperand(array);
  if (!needResult) {
    return nullptr;
  }
  codegen_.refCounts().retainValue(value, element);

  return value;
}

// `a.length = n`
llvm::Value *ArrayLowerer::emitLengthAssign(AssignExpr *assign, bool needResult) {
  auto *member = cast<MemberExpr>(assign->target);
  CodeGen::Operand array = codegen_.refCounts().emitOperand(member->object, AssignLowerer::cannotAssign(assign->value));
  codegen_.throwLowerer().holdOperand(array, codegen_.mayThrow().exprThrows(assign->value));
  llvm::Value *length = codegen_.exprLowerer().emitExpr(assign->value);
  codegen_.throwLowerer().takeOperand(array);
  codegen_.builder_.CreateCall(codegen_.rt_.arrSetLength, {array.value, length});
  codegen_.refCounts().dropOperand(array);
  // A negative length, or one that would grow an array whose elements have
  // no default.
  codegen_.throwLowerer().emitThrowCheck();

  return needResult ? length : nullptr;
}

// The loop of `for...of` over an array or a string (see emitForOf).
void ArrayLowerer::emitArrayForOf(ForOfStmt *stmt, llvm::Value *iterableSlot,
                             llvm::BasicBlock *condBlock, llvm::BasicBlock *bodyBlock,
                             llvm::BasicBlock *endBlock) {
  const Type *iterableType = stmt->iterable->type;
  bool isString = iterableType->isString();
  // A loop variable a closure keeps in a box holds its own reference.
  bool borrow = stmt->borrowElements && !stmt->binding.sym->boxed();
  const Type *element = isString ? iterableType : iterableType->inner();
  llvm::Type *i64 = codegen_.builder_.getInt64Ty();
  llvm::Value *indexSlot = codegen_.functionFrame().createStackSlot(i64, "for.index");
  codegen_.builder_.CreateStore(codegen_.builder_.getInt64(0), indexSlot);

  codegen_.functionFrame().emitBlock(condBlock);
  // Each element is read here, in whatever piece the iteration is.
  codegen_.orderAt(stmt->loc());
  codegen_.orderAt(stmt->iterable->loc());
  llvm::Value *iterable = codegen_.builder_.CreateLoad(codegen_.moduleBuilder().lower(iterableType), iterableSlot);
  llvm::Value *index = codegen_.builder_.CreateLoad(i64, indexSlot, "index");
  llvm::Value *length = isString ? codegen_.stringLowerer().stringLength(iterable) : arrayLength(iterable);
  codegen_.builder_.CreateCondBr(codegen_.builder_.CreateICmpULT(index, length), bodyBlock, endBlock);

  codegen_.functionFrame().switchTo(bodyBlock);
  codegen_.functionFrame().pushScope(); // the loop variable, fresh in every iteration
  llvm::Value *value;
  if (isString) {
    // One code point: one code unit, or two for a surrogate pair.
    llvm::Value *out = codegen_.functionFrame().createStackSlot(codegen_.strTy_, "code.point");
    llvm::Value *units = codegen_.builder_.CreateCall(
        codegen_.rt_.strCodePointAt, {out, codegen_.stringLowerer().stringObj(iterable), codegen_.stringLowerer().stringMeta(iterable), index});
    codegen_.builder_.CreateStore(codegen_.builder_.CreateAdd(index, codegen_.builder_.CreateSExt(units, i64)), indexSlot);
    value = codegen_.builder_.CreateLoad(codegen_.strTy_, out);
  } else {
    value =
        codegen_.builder_.CreateLoad(codegen_.moduleBuilder().lower(element), elementAddress(arrayData(iterable), element, index));
    if (!borrow) {
      codegen_.refCounts().retainValue(value, element);
    }
    codegen_.builder_.CreateStore(codegen_.builder_.CreateAdd(index, codegen_.builder_.getInt64(1)), indexSlot);
  }
  if (borrow) {
    // A plain slot: nothing to release when the iteration ends.
    llvm::Value *slot = codegen_.functionFrame().createStackSlot(codegen_.moduleBuilder().lower(element), stmt->binding.name);
    codegen_.addresses_[stmt->binding.sym] = slot;
    codegen_.builder_.CreateStore(value, slot);
  } else {
    codegen_.storeLowerer().storeVar(stmt->binding.sym, codegen_.functionFrame().createLocal(stmt->binding.sym, stmt->binding.name), value,
             /*replacing=*/true);
  }

  codegen_.loopLowerer().pushLoop(endBlock, condBlock, codegen_.scopes_.size() - 1);
  codegen_.stmtLowerer().emitStmt(stmt->body);
  codegen_.targets_.pop_back();
  codegen_.functionFrame().popScope();
  codegen_.functionFrame().branchTo(condBlock);
}

} // namespace tz
