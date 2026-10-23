// Arrays: literals, element access, methods and `for...of`.
//
// An array value is a pointer to a header (see runtime/array.c):
//   { i64 rc, ptr type, ptr data, i32 length, i32 capacity, ptr alloc }
// `type` describes the elements for the runtime, `data` points at the first
// element. Elements are stored unboxed by their static type. The hot paths
// (indexing, length, push, pop) are emitted inline; the rest calls the runtime.

#include "codegen/codegen.h"

#include "llvm/IR/MDBuilder.h"
#include "llvm/Support/ErrorHandling.h"

using llvm::cast;
using llvm::dyn_cast;
using llvm::isa;

namespace tz {

namespace {

// Header fields; must match runtime/array.c.
enum ArrayField : unsigned { kRefCount, kElementType, kData, kLength, kCapacity, kAlloc };

// Type descriptor kinds; must match runtime/tzrt.h.
uint32_t descriptorKind(const Type *type) {
  switch (type->kind()) {
  case Type::Kind::Int: return 1;
  case Type::Kind::Long: return 2;
  case Type::Kind::Double: return 3;
  case Type::Kind::Bool: return 4;
  case Type::Kind::String: return 5;
  case Type::Kind::Array: return 6;
  case Type::Kind::Optional: return 7;
  case Type::Kind::Undefined: return 8;
  case Type::Kind::Function: return 9;
  case Type::Kind::Object: return 10;
  default: llvm_unreachable("no runtime descriptor for this type");
  }
}

} // namespace

// A constant { i32 kind, i32 size, ptr inner } describing a type to the
// runtime: how big its values are and how to release, compare and print them.
llvm::Constant *CodeGen::typeDescriptor(const Type *type) {
  if (llvm::Constant *found = typeDescriptors_.lookup(type))
    return found;
  // The element of an array, the value of `T | undefined`; a function's
  // inner() is its return type, which is not part of the value.
  llvm::Constant *inner = type->isArray() || type->isOptional()
                              ? typeDescriptor(type->inner())
                              : llvm::ConstantPointerNull::get(builder_.getPtrTy());
  uint64_t size = module_->getDataLayout().getTypeAllocSize(lower(type));
  llvm::Constant *init = llvm::ConstantStruct::get(
      typeDescTy_, {builder_.getInt32(descriptorKind(type)), builder_.getInt32(static_cast<uint32_t>(size)),
                    inner});
  auto *global = new llvm::GlobalVariable(*module_, typeDescTy_, /*isConstant=*/true,
                                          llvm::GlobalValue::PrivateLinkage, init,
                                          "tz.type." + type->name());
  global->setUnnamedAddr(llvm::GlobalValue::UnnamedAddr::Global);
  typeDescriptors_[type] = global;
  return global;
}

llvm::Value *CodeGen::arrayField(llvm::Value *array, unsigned field) {
  return builder_.CreateStructGEP(arrayTy_, array, field);
}

llvm::Value *CodeGen::arrayLength(llvm::Value *array) {
  llvm::Value *length = builder_.CreateLoad(builder_.getInt32Ty(), arrayField(array, kLength), "length");
  return builder_.CreateZExt(length, builder_.getInt64Ty());
}

llvm::Value *CodeGen::arrayData(llvm::Value *array) {
  return builder_.CreateLoad(builder_.getPtrTy(), arrayField(array, kData), "data");
}

llvm::Value *CodeGen::elementAddress(llvm::Value *data, const Type *element, llvm::Value *index) {
  return builder_.CreateInBoundsGEP(lower(element), data, index);
}

llvm::Value *CodeGen::emitCheckedRead(llvm::Value *inRange, const Type *resultType,
                                      llvm::function_ref<llvm::Value *()> read) {
  llvm::BasicBlock *before = builder_.GetInsertBlock();
  llvm::BasicBlock *readBlock = newBlock("read");
  llvm::BasicBlock *endBlock = newBlock("read.end");
  builder_.CreateCondBr(inRange, readBlock, endBlock);

  switchTo(readBlock);
  // An element that is itself `T | undefined` is the result as it is.
  llvm::Value *value = read();
  if (value->getType() != lower(resultType))
    value = makeOptional(value, builder_.getTrue(), resultType);
  llvm::BasicBlock *readEnd = builder_.GetInsertBlock();
  builder_.CreateBr(endBlock);

  switchTo(endBlock);
  llvm::PHINode *phi = builder_.CreatePHI(lower(resultType), 2);
  phi->addIncoming(undefinedValue(resultType), before);
  phi->addIncoming(value, readEnd);
  return phi;
}

// Appends owned values; the array takes them over.
void CodeGen::emitPush(llvm::Value *array, llvm::ArrayRef<llvm::Value *> values, const Type *element) {
  llvm::Type *i32 = builder_.getInt32Ty();
  llvm::Value *length = builder_.CreateLoad(i32, arrayField(array, kLength), "length");
  llvm::Value *capacity = builder_.CreateLoad(i32, arrayField(array, kCapacity), "capacity");
  llvm::Value *newLength = builder_.CreateAdd(length, builder_.getInt32(static_cast<uint32_t>(values.size())));
  llvm::BasicBlock *growBlock = newBlock("push.grow");
  llvm::BasicBlock *storeBlock = newBlock("push.store");
  builder_.CreateCondBr(builder_.CreateICmpUGT(newLength, capacity), growBlock, storeBlock,
                        llvm::MDBuilder(ctx_).createUnlikelyBranchWeights());
  switchTo(growBlock);
  builder_.CreateCall(rt_.arrGrow, {array, builder_.CreateZExt(newLength, builder_.getInt64Ty())});
  builder_.CreateBr(storeBlock);

  switchTo(storeBlock);
  llvm::Value *data = arrayData(array);
  llvm::Value *at = builder_.CreateZExt(length, builder_.getInt64Ty());
  for (size_t i = 0; i < values.size(); ++i)
    builder_.CreateStore(values[i], elementAddress(data, element, builder_.CreateAdd(at, builder_.getInt64(i))));
  builder_.CreateStore(newLength, arrayField(array, kLength));
}

llvm::Value *CodeGen::emitArrayLiteral(ArrayLitExpr *lit) {
  const Type *element = lit->type->inner();
  llvm::Constant *descriptor = typeDescriptor(element);
  bool spreads = llvm::any_of(lit->elements, [](const ArrayLitExpr::Element &e) { return e.spread; });

  if (!spreads) {
    llvm::Value *count = builder_.getInt64(lit->elements.size());
    llvm::Value *array = builder_.CreateCall(rt_.arrNew, {descriptor, count, count}, "array");
    if (lit->elements.empty())
      return array;
    // Nothing can reach the new array while its elements are evaluated, so
    // its data pointer stays put.
    llvm::Value *data = arrayData(array);
    for (size_t i = 0; i < lit->elements.size(); ++i)
      builder_.CreateStore(emitExpr(lit->elements[i].expr),
                           elementAddress(data, element, builder_.getInt64(i)));
    return array;
  }

  size_t plain = llvm::count_if(lit->elements, [](const ArrayLitExpr::Element &e) { return !e.spread; });
  llvm::Value *array = builder_.CreateCall(
      rt_.arrNew, {descriptor, builder_.getInt64(0), builder_.getInt64(plain)}, "array");
  for (const ArrayLitExpr::Element &e : lit->elements) {
    if (e.spread) {
      // Appended right away, so a variable can be borrowed. A string adds
      // its characters.
      Operand source = emitOperand(e.expr, /*canBorrow=*/true);
      if (e.expr->type->isString())
        builder_.CreateCall(rt_.arrAppendCodePoints,
                            {array, stringObj(source.value), stringMeta(source.value)});
      else
        builder_.CreateCall(rt_.arrAppend, {array, source.value});
      dropOperand(source);
    } else {
      emitPush(array, {emitExpr(e.expr)}, element);
    }
  }
  return array;
}

llvm::Value *CodeGen::emitArrayFold(llvm::Value *array, const Type *element, llvm::Value *init,
                                    llvm::function_ref<llvm::Value *(llvm::Value *, llvm::Value *)> step) {
  llvm::BasicBlock *before = builder_.GetInsertBlock();
  llvm::Value *length = arrayLength(array);
  llvm::BasicBlock *headBlock = newBlock("fold");
  llvm::BasicBlock *bodyBlock = newBlock("fold.body");
  llvm::BasicBlock *endBlock = newBlock("fold.end");
  builder_.CreateBr(headBlock);
  switchTo(headBlock);
  llvm::PHINode *i = builder_.CreatePHI(builder_.getInt64Ty(), 2, "i");
  llvm::PHINode *acc = builder_.CreatePHI(init->getType(), 2, "acc");
  i->addIncoming(builder_.getInt64(0), before);
  acc->addIncoming(init, before);
  builder_.CreateCondBr(builder_.CreateICmpULT(i, length), bodyBlock, endBlock);
  switchTo(bodyBlock);
  llvm::Value *value = builder_.CreateLoad(lower(element), elementAddress(arrayData(array), element, i));
  llvm::Value *next = step(acc, value);
  i->addIncoming(builder_.CreateAdd(i, builder_.getInt64(1)), builder_.GetInsertBlock());
  acc->addIncoming(next, builder_.GetInsertBlock());
  builder_.CreateBr(headBlock);
  switchTo(endBlock);
  return acc;
}

llvm::Value *CodeGen::emitNewArray(NewExpr *expr) {
  llvm::Constant *descriptor = typeDescriptor(expr->type->inner());
  if (expr->args.empty())
    return builder_.CreateCall(rt_.arrNew, {descriptor, builder_.getInt64(0), builder_.getInt64(0)},
                               "array");
  // The runtime reports a negative length, as JavaScript's RangeError would.
  return builder_.CreateCall(rt_.arrNewLength, {descriptor, emitExpr(expr->args[0])}, "array");
}

llvm::Value *CodeGen::emitArrayIndex(IndexExpr *index) {
  const Type *element = index->object->type->inner();
  Operand array = emitOperand(index->object, cannotAssign(index->index));
  llvm::Value *position = emitExpr(index->index);
  auto read = [&] {
    llvm::Value *value = builder_.CreateLoad(lower(element),
                                             elementAddress(arrayData(array.value), element, position));
    retainValue(value, element);
    return value;
  };
  llvm::Value *result;
  if (!index->mayBeOutOfRange)
    result = read();
  else
    result = emitCheckedRead(builder_.CreateICmpULT(position, arrayLength(array.value)), index->type, read);
  dropOperand(array);
  return result;
}

// `a[i] = v` and `a[i] op= v`. The store happens after v is evaluated, which
// may have changed the array, so the data pointer is loaded only then.
llvm::Value *CodeGen::emitElementAssign(AssignExpr *assign, bool needResult) {
  auto *index = cast<IndexExpr>(assign->target);
  const Type *element = index->type;
  Operand array = emitOperand(index->object, cannotAssign(index->index) && cannotAssign(assign->value));
  llvm::Value *position = emitExpr(index->index);

  llvm::Value *value;
  if (!assign->op) {
    value = emitExpr(assign->value);
  } else {
    // As in JavaScript, the old value is read before the right side runs.
    llvm::Value *old = builder_.CreateLoad(lower(element), elementAddress(arrayData(array.value), element, position));
    if (element->isString()) {
      retainString(old); // the right side may replace the element
      Operand rhs = emitConcatPart(assign->value, /*canBorrow=*/false);
      value = concatOperands({{old, true, element}, rhs});
    } else {
      value = emitArithmetic(*assign->op, old, emitExpr(assign->value), element);
    }
  }

  auto replace = [&] {
    storeReplacing(elementAddress(arrayData(array.value), element, position), value, element);
  };
  if (!index->mayAppend) {
    replace();
  } else {
    // Sema proved index <= length: either a replacement or an append.
    llvm::BasicBlock *replaceBlock = newBlock("store.replace");
    llvm::BasicBlock *appendBlock = newBlock("store.append");
    llvm::BasicBlock *endBlock = newBlock("store.end");
    builder_.CreateCondBr(builder_.CreateICmpULT(position, arrayLength(array.value)), replaceBlock,
                          appendBlock);
    switchTo(replaceBlock);
    replace();
    builder_.CreateBr(endBlock);
    switchTo(appendBlock);
    emitPush(array.value, {value}, element);
    builder_.CreateBr(endBlock);
    switchTo(endBlock);
  }
  dropOperand(array);
  if (!needResult)
    return nullptr;
  retainValue(value, element);
  return value;
}

// `a.length = n`
llvm::Value *CodeGen::emitLengthAssign(AssignExpr *assign, bool needResult) {
  auto *member = cast<MemberExpr>(assign->target);
  Operand array = emitOperand(member->object, cannotAssign(assign->value));
  llvm::Value *length = emitExpr(assign->value);
  builder_.CreateCall(rt_.arrSetLength, {array.value, length});
  dropOperand(array);
  return needResult ? length : nullptr;
}

llvm::Value *CodeGen::emitArrayMethod(CallExpr *call) {
  auto *member = cast<MemberExpr>(call->callee);
  const Type *arrayType = member->object->type;
  const Type *element = arrayType->inner();
  Operand array = emitOperand(member->object, llvm::all_of(call->args, cannotAssign));

  // All arguments are evaluated before the method runs, as in JavaScript.
  auto ownedArgs = [&](size_t from) {
    std::vector<llvm::Value *> values;
    for (size_t i = from; i < call->args.size(); ++i)
      values.push_back(emitExpr(call->args[i]));
    return values;
  };
  // An optional position argument: its value and whether it was given.
  auto position = [&](llvm::ArrayRef<llvm::Value *> values, size_t i) -> std::pair<llvm::Value *, llvm::Value *> {
    if (i < values.size())
      return {values[i], builder_.getInt32(1)};
    return {builder_.getInt64(0), builder_.getInt32(0)};
  };
  // Methods that return the array itself hand over the receiver's reference.
  auto returnArray = [&] {
    if (!array.owned)
      retainValue(array.value, arrayType);
    array.owned = false;
    return array.value;
  };

  llvm::Value *result = nullptr;
  switch (call->builtin) {
  case Builtin::ArrPush: {
    std::vector<llvm::Value *> values = ownedArgs(0);
    // `a.push(x, ...b)`: the arrays spread are appended where they stand.
    std::vector<llvm::Value *> run;
    for (size_t i = 0; i < values.size(); ++i) {
      if (!call->spreads(i)) {
        run.push_back(values[i]);
        continue;
      }
      if (!run.empty())
        emitPush(array.value, run, element);
      run.clear();
      builder_.CreateCall(rt_.arrAppend, {array.value, values[i]});
      releaseValue(values[i], arrayType);
    }
    if (!run.empty())
      emitPush(array.value, run, element);
    result = builder_.CreateTrunc(arrayLength(array.value), builder_.getInt32Ty());
    break;
  }
  case Builtin::ArrUnshift: {
    std::vector<llvm::Value *> values = ownedArgs(0);
    llvm::Value *slots = builder_.CreateCall(rt_.arrUnshiftSlots, {array.value, builder_.getInt64(values.size())});
    for (size_t i = 0; i < values.size(); ++i)
      builder_.CreateStore(values[i], elementAddress(slots, element, builder_.getInt64(i)));
    result = builder_.CreateTrunc(arrayLength(array.value), builder_.getInt32Ty());
    break;
  }
  case Builtin::ArrPop: {
    // The element moves out of the array: no counting needed.
    llvm::Value *length = arrayLength(array.value);
    auto pop = [&] {
      llvm::Value *last = builder_.CreateSub(length, builder_.getInt64(1));
      llvm::Value *value = builder_.CreateLoad(lower(element), elementAddress(arrayData(array.value), element, last));
      builder_.CreateStore(builder_.CreateTrunc(last, builder_.getInt32Ty()), arrayField(array.value, kLength));
      return value;
    };
    result = call->mayBeOutOfRange
                 ? emitCheckedRead(builder_.CreateICmpNE(length, builder_.getInt64(0)), call->type, pop)
                 : pop();
    break;
  }
  case Builtin::ArrShift: {
    llvm::Value *slot = createStackSlot(lower(element), "shifted");
    llvm::Value *has = builder_.CreateCall(rt_.arrShift, {array.value, slot});
    llvm::Value *value = builder_.CreateLoad(lower(element), slot);
    result = call->mayBeOutOfRange
                 ? makeOptional(value, builder_.CreateICmpNE(has, builder_.getInt32(0)), call->type)
                 : value;
    break;
  }
  case Builtin::ArrAt: {
    llvm::Value *length = arrayLength(array.value);
    llvm::Value *at = emitExpr(call->args[0]);
    at = builder_.CreateSelect(builder_.CreateICmpSLT(at, builder_.getInt64(0)),
                               builder_.CreateAdd(at, length), at);
    auto read = [&] {
      llvm::Value *value = builder_.CreateLoad(lower(element), elementAddress(arrayData(array.value), element, at));
      retainValue(value, element);
      return value;
    };
    result = call->mayBeOutOfRange ? emitCheckedRead(builder_.CreateICmpULT(at, length), call->type, read)
                                   : read();
    break;
  }
  case Builtin::ArrSlice: {
    std::vector<llvm::Value *> values = ownedArgs(0);
    llvm::Value *start = values.empty() ? builder_.getInt64(0) : values[0];
    auto [end, hasEnd] = position(values, 1);
    result = builder_.CreateCall(rt_.arrSlice, {array.value, start, end, hasEnd}, "slice");
    break;
  }
  case Builtin::ArrConcat: {
    std::vector<Operand> parts;
    for (size_t i = 0; i < call->args.size(); ++i)
      parts.push_back({emitExpr(call->args[i]), true, call->args[i]->type});
    result = builder_.CreateCall(rt_.arrCopy, {array.value}, "concat");
    for (Operand &part : parts) {
      if (part.type == arrayType) {
        builder_.CreateCall(rt_.arrAppend, {result, part.value});
        dropOperand(part);
      } else {
        emitPush(result, {part.value}, element); // the array takes ownership
      }
    }
    break;
  }
  case Builtin::ArrIndexOf:
  case Builtin::ArrLastIndexOf:
  case Builtin::ArrIncludes: {
    std::vector<llvm::Value *> values = ownedArgs(0);
    auto [from, hasFrom] = position(values, 1);
    int mode = call->builtin == Builtin::ArrIndexOf ? 0 : call->builtin == Builtin::ArrLastIndexOf ? 1 : 2;
    result = builder_.CreateCall(rt_.arrIndexOf, {array.value, spill(values[0], "search"), from, hasFrom,
                                                  builder_.getInt32(mode)});
    releaseValue(values[0], element);
    if (mode == 2)
      result = builder_.CreateICmpNE(result, builder_.getInt32(0));
    break;
  }
  case Builtin::ArrJoin:
  case Builtin::ArrToString: {
    if (call->args.empty()) {
      llvm::Value *none = llvm::ConstantPointerNull::get(builder_.getPtrTy());
      result = callReturningString(rt_.arrJoin, {array.value, none, builder_.getInt64(0), builder_.getInt32(0)});
    } else {
      Operand separator = emitOperand(call->args[0], /*canBorrow=*/true);
      result = callReturningString(rt_.arrJoin, {array.value, stringObj(separator.value),
                                                 stringMeta(separator.value), builder_.getInt32(1)});
      dropOperand(separator);
    }
    break;
  }
  case Builtin::ArrReverse:
    builder_.CreateCall(rt_.arrReverse, {array.value});
    result = returnArray();
    break;
  case Builtin::ArrSort:
    builder_.CreateCall(rt_.arrSort, {array.value});
    result = returnArray();
    break;
  case Builtin::ArrFill: {
    std::vector<llvm::Value *> values = ownedArgs(0);
    auto [start, hasStart] = position(values, 1);
    auto [end, hasEnd] = position(values, 2);
    builder_.CreateCall(rt_.arrFill, {array.value, spill(values[0], "fill"), start, end, hasStart, hasEnd});
    releaseValue(values[0], element);
    result = returnArray();
    break;
  }
  case Builtin::ArrSplice: {
    // The runtime moves the removed elements into a new array and opens a
    // gap for the inserted ones, which take over the values.
    std::vector<llvm::Value *> values = ownedArgs(0);
    auto [count, hasCount] = position(values, 1);
    size_t inserted = values.size() > 2 ? values.size() - 2 : 0;
    llvm::Value *slotsOut = createStackSlot(builder_.getPtrTy(), "splice.slots");
    result = builder_.CreateCall(rt_.arrSplice, {array.value, values[0], count, hasCount,
                                                 builder_.getInt64(inserted), slotsOut},
                                 "removed");
    llvm::Value *slots = builder_.CreateLoad(builder_.getPtrTy(), slotsOut);
    for (size_t i = 0; i < inserted; ++i)
      builder_.CreateStore(values[i + 2], elementAddress(slots, element, builder_.getInt64(i)));
    break;
  }
  default:
    llvm_unreachable("not an array method");
  }
  dropOperand(array);
  return result;
}

// The loop of `for...of` over an array or a string (see emitForOf).
void CodeGen::emitArrayForOf(ForOfStmt *stmt, llvm::Value *iterableSlot, llvm::BasicBlock *condBlock,
                             llvm::BasicBlock *bodyBlock, llvm::BasicBlock *endBlock) {
  const Type *iterableType = stmt->iterable->type;
  bool isString = iterableType->isString();
  // A loop variable a closure keeps in a box holds its own reference.
  bool borrow = stmt->borrowElements && !stmt->binding.sym->boxed();
  const Type *element = isString ? iterableType : iterableType->inner();
  llvm::Type *i64 = builder_.getInt64Ty();
  llvm::Value *indexSlot = createStackSlot(i64, "for.index");
  builder_.CreateStore(builder_.getInt64(0), indexSlot);

  emitBlock(condBlock);
  llvm::Value *iterable = builder_.CreateLoad(lower(iterableType), iterableSlot);
  llvm::Value *index = builder_.CreateLoad(i64, indexSlot, "index");
  llvm::Value *length = isString ? stringLength(iterable) : arrayLength(iterable);
  builder_.CreateCondBr(builder_.CreateICmpULT(index, length), bodyBlock, endBlock);

  switchTo(bodyBlock);
  pushScope(); // the loop variable, fresh in every iteration
  llvm::Value *value;
  if (isString) {
    // One code point: one code unit, or two for a surrogate pair.
    llvm::Value *out = createStackSlot(strTy_, "code.point");
    llvm::Value *units = builder_.CreateCall(rt_.strCodePointAt,
                                             {out, stringObj(iterable), stringMeta(iterable), index});
    builder_.CreateStore(builder_.CreateAdd(index, builder_.CreateSExt(units, i64)), indexSlot);
    value = builder_.CreateLoad(strTy_, out);
  } else {
    value = builder_.CreateLoad(lower(element), elementAddress(arrayData(iterable), element, index));
    if (!borrow)
      retainValue(value, element);
    builder_.CreateStore(builder_.CreateAdd(index, builder_.getInt64(1)), indexSlot);
  }
  if (borrow) {
    // A plain slot: nothing to release when the iteration ends.
    llvm::Value *slot = createStackSlot(lower(element), stmt->binding.name);
    addresses_[stmt->binding.sym] = slot;
    builder_.CreateStore(value, slot);
  } else {
    storeReplacing(createLocal(stmt->binding.sym, stmt->binding.name), value, element);
  }

  pushLoop(endBlock, condBlock, scopes_.size() - 1);
  emitStmt(stmt->body);
  targets_.pop_back();
  popScope();
  branchTo(condBlock);
}

// forEach, map, filter, flatMap, reduce, reduceRight, find, findIndex,
// findLast, findLastIndex, some and every. The array is evaluated once and
// stays alive during the call, and the callback gets that array, whatever it
// does meanwhile. As in JavaScript, the elements visited are those below the
// length the array had at the start that are still there when their turn
// comes; elements the callback adds are not visited.
llvm::Value *CodeGen::emitCallbackMethod(CallExpr *call) {
  auto *member = cast<MemberExpr>(call->callee);
  const Type *arrayType = member->object->type;
  const Type *element = arrayType->inner();
  const Type *callbackType = call->args[0]->type;
  llvm::Type *i64 = builder_.getInt64Ty();
  Builtin method = call->builtin;

  llvm::Value *array = emitExpr(member->object);
  llvm::Value *callback = emitExpr(call->args[0]);
  llvm::Value *init = call->args.size() > 1 ? emitExpr(call->args[1]) : nullptr;

  if (method == Builtin::ArrSortWith) {
    // The runtime sorts; the comparison goes through a function that calls
    // the callback.
    llvm::Value *context = spill(callback, "compare");
    builder_.CreateCall(rt_.arrSortWith, {array, sortComparator(call), context});
    releaseValue(callback, callbackType);
    return array; // the same array, with this reference
  }

  bool reduce = method == Builtin::ArrReduce || method == Builtin::ArrReduceRight;
  bool backward = method == Builtin::ArrReduceRight || method == Builtin::ArrFindLast ||
                  method == Builtin::ArrFindLastIndex;
  size_t passes = std::min(callbackType->params().size(), size_t(reduce ? 4 : 3));
  // The arguments of the callback that are the element (and the accumulator).
  size_t valueArgs = reduce ? 2 : 1;

  llvm::Value *length = arrayLength(array);
  llvm::Value *start = backward ? builder_.CreateSub(length, builder_.getInt64(1)) : builder_.getInt64(0);
  llvm::Value *step = builder_.getInt64(backward ? -1 : 1);
  auto readElement = [&](llvm::Value *at) {
    return builder_.CreateLoad(lower(element), elementAddress(arrayData(array), element, at), "element");
  };

  // reduce: the accumulator, from the initial value or the first element.
  llvm::Value *accSlot = nullptr;
  if (reduce) {
    accSlot = createStackSlot(lower(call->type), "acc");
    if (init) {
      builder_.CreateStore(init, accSlot);
    } else {
      llvm::BasicBlock *emptyBlock = newBlock("reduce.empty");
      llvm::BasicBlock *firstBlock = newBlock("reduce.first");
      builder_.CreateCondBr(builder_.CreateICmpEQ(length, builder_.getInt64(0)), emptyBlock, firstBlock,
                            llvm::MDBuilder(ctx_).createUnlikelyBranchWeights());
      switchTo(emptyBlock);
      emitPanic("TypeError: Reduce of empty array with no initial value");
      switchTo(firstBlock);
      llvm::Value *first = readElement(start);
      retainValue(first, element);
      builder_.CreateStore(first, accSlot);
      start = builder_.CreateAdd(start, step);
    }
  }
  // map, filter, flatMap: the new array.
  llvm::Value *out = nullptr;
  if (method == Builtin::ArrMap || method == Builtin::ArrFilter || method == Builtin::ArrFlatMap)
    out = builder_.CreateCall(rt_.arrNew, {typeDescriptor(call->type->inner()), builder_.getInt64(0),
                                           method == Builtin::ArrMap ? length : builder_.getInt64(0)},
                              "result");
  // find, findIndex, some, every: the result if no element decides it.
  llvm::Value *resultSlot = nullptr;
  if (!reduce && !out && method != Builtin::ArrForEach) {
    resultSlot = createStackSlot(lower(call->type), "result");
    llvm::Value *none = method == Builtin::ArrSome    ? builder_.getFalse()
                        : method == Builtin::ArrEvery ? builder_.getTrue()
                        : call->type->isInt()         ? builder_.getInt32(~0u)
                                                      : undefinedValue(call->type);
    builder_.CreateStore(none, resultSlot);
  }

  llvm::Value *indexSlot = createStackSlot(i64, "index");
  builder_.CreateStore(start, indexSlot);
  llvm::BasicBlock *headBlock = newBlock("each");
  llvm::BasicBlock *bodyBlock = newBlock("each.body");
  llvm::BasicBlock *nextBlock = newBlock("each.next");
  llvm::BasicBlock *endBlock = newBlock("each.end");
  emitBlock(headBlock);
  llvm::Value *i = builder_.CreateLoad(i64, indexSlot, "i");
  // Below the length at the start (or not below 0 going back), and still in
  // the array. Going back, the unsigned comparison covers both.
  llvm::Value *inRange = builder_.CreateICmpULT(i, arrayLength(array));
  if (!backward)
    inRange = builder_.CreateAnd(inRange, builder_.CreateICmpULT(i, length));
  builder_.CreateCondBr(inRange, bodyBlock, endBlock);

  switchTo(bodyBlock);
  llvm::Value *value = readElement(i);
  // The callback owns its arguments; filter and find keep one more
  // reference to the element, to put it in the result.
  bool keeps = method == Builtin::ArrFilter || method == Builtin::ArrFind || method == Builtin::ArrFindLast;
  if (passes >= valueArgs)
    retainValue(value, element);
  if (keeps)
    retainValue(value, element);
  std::vector<llvm::Value *> args;
  if (reduce)
    args.push_back(builder_.CreateLoad(lower(call->type), accSlot)); // moves into the call
  args.push_back(value);
  args.push_back(builder_.CreateTrunc(i, builder_.getInt32Ty()));
  args.push_back(array);
  // The accumulator moves into the call, unless the callback takes none.
  if (reduce && passes == 0)
    releaseValue(args[0], call->type);
  args.resize(passes);
  if (passes == valueArgs + 2)
    retainValue(array, arrayType);
  llvm::Value *result = callFunctionValue(callback, callbackType, args);
  const Type *resultType = callbackType->ret();

  // What the result means. `found` is where an element decides the result.
  llvm::BasicBlock *foundBlock = nullptr;
  auto test = [&](llvm::Value *decides) {
    foundBlock = newBlock("each.found");
    llvm::BasicBlock *otherBlock = newBlock("each.other");
    builder_.CreateCondBr(decides, foundBlock, otherBlock);
    switchTo(otherBlock);
  };
  switch (method) {
  case Builtin::ArrForEach:
    if (result)
      releaseValue(result, resultType);
    break;
  case Builtin::ArrMap:
    // One result per element visited, in order: the new array has room for
    // them all.
    builder_.CreateStore(result, elementAddress(arrayData(out), resultType, i));
    builder_.CreateStore(builder_.CreateTrunc(builder_.CreateAdd(i, builder_.getInt64(1)), builder_.getInt32Ty()),
                         arrayField(out, kLength));
    break;
  case Builtin::ArrFlatMap:
    if (resultType->isArray()) {
      builder_.CreateCall(rt_.arrAppend, {out, result});
      releaseValue(result, resultType);
    } else {
      emitPush(out, {result}, resultType);
    }
    break;
  case Builtin::ArrFilter: {
    llvm::BasicBlock *keepBlock = newBlock("filter.keep");
    llvm::BasicBlock *dropBlock = newBlock("filter.drop");
    builder_.CreateCondBr(result, keepBlock, dropBlock);
    switchTo(keepBlock);
    emitPush(out, {value}, element);
    builder_.CreateBr(nextBlock);
    switchTo(dropBlock);
    releaseValue(value, element);
    break;
  }
  case Builtin::ArrReduce:
  case Builtin::ArrReduceRight:
    builder_.CreateStore(result, accSlot);
    break;
  case Builtin::ArrFind:
  case Builtin::ArrFindLast:
    test(result);
    releaseValue(value, element);
    builder_.CreateBr(nextBlock);
    switchTo(foundBlock);
    builder_.CreateStore(makeOptional(value, builder_.getTrue(), call->type), resultSlot);
    builder_.CreateBr(endBlock);
    switchTo(newBlock("dead"));
    break;
  case Builtin::ArrFindIndex:
  case Builtin::ArrFindLastIndex:
    test(result);
    builder_.CreateBr(nextBlock);
    switchTo(foundBlock);
    builder_.CreateStore(builder_.CreateTrunc(i, builder_.getInt32Ty()), resultSlot);
    builder_.CreateBr(endBlock);
    switchTo(newBlock("dead"));
    break;
  case Builtin::ArrSome:
  case Builtin::ArrEvery: {
    bool some = method == Builtin::ArrSome;
    test(some ? result : builder_.CreateNot(result));
    builder_.CreateBr(nextBlock);
    switchTo(foundBlock);
    builder_.CreateStore(builder_.getInt1(some), resultSlot);
    builder_.CreateBr(endBlock);
    switchTo(newBlock("dead"));
    break;
  }
  default:
    llvm_unreachable("not a method with a callback");
  }
  branchTo(nextBlock);

  switchTo(nextBlock);
  builder_.CreateStore(builder_.CreateAdd(i, step), indexSlot);
  builder_.CreateBr(headBlock);

  switchTo(endBlock);
  releaseValue(callback, callbackType);
  releaseValue(array, arrayType);
  if (out)
    return out;
  if (reduce)
    return builder_.CreateLoad(lower(call->type), accSlot);
  if (resultSlot)
    return builder_.CreateLoad(lower(call->type), resultSlot);
  return nullptr;
}

// The function the runtime calls to compare two elements for sort(compare):
// (ptr context, ptr x, ptr y) -> i32, the sign of compare(*x, *y), where
// the context holds the function value. NaN counts as 0.
llvm::Function *CodeGen::sortComparator(CallExpr *call) {
  const Type *element = cast<MemberExpr>(call->callee)->object->type->inner();
  Expr *callbackExpr = call->args[0];
  const Type *callbackType = callbackExpr->type;
  llvm::Type *ptr = builder_.getPtrTy();
  auto *type = llvm::FunctionType::get(builder_.getInt32Ty(), {ptr, ptr, ptr}, false);
  auto *fn = llvm::Function::Create(type, llvm::GlobalValue::InternalLinkage, "tz.compare", module_.get());
  setFunctionAttributes(fn);
  llvm::IRBuilderBase::InsertPoint point = builder_.saveIP();
  llvm::Function *outer = function_;
  function_ = fn;
  builder_.SetInsertPoint(llvm::BasicBlock::Create(ctx_, "entry", fn));
  llvm::Value *callback = builder_.CreateLoad(fnTy_, fn->getArg(0), "compare");
  size_t passes = std::min<size_t>(callbackType->params().size(), 2);
  std::vector<llvm::Value *> args;
  for (size_t k = 0; k < passes; ++k) {
    llvm::Value *value = builder_.CreateLoad(lower(element), fn->getArg(unsigned(k + 1)));
    retainValue(value, element); // the callback owns its arguments
    args.push_back(value);
  }
  llvm::Value *result;
  if (auto *fe = dyn_cast<FunctionExpr>(callbackExpr)) {
    // An arrow written here: its code is known, and may be inlined.
    std::vector<llvm::Value *> all{builder_.CreateExtractValue(callback, 1)};
    all.insert(all.end(), args.begin(), args.end());
    for (size_t k = passes; k < callbackType->params().size(); ++k)
      all.push_back(undefinedValue(callbackType->params()[k]));
    result = builder_.CreateCall(closureCode(fe->fn), all);
  } else {
    result = callFunctionValue(callback, callbackType, args);
  }
  const Type *resultType = callbackType->ret();
  llvm::Value *positive, *negative;
  if (resultType->isFloat()) {
    llvm::Value *zero = llvm::ConstantFP::get(builder_.getDoubleTy(), 0.0);
    positive = builder_.CreateFCmpOGT(result, zero);
    negative = builder_.CreateFCmpOLT(result, zero);
  } else {
    llvm::Value *zero = llvm::ConstantInt::get(result->getType(), 0);
    positive = builder_.CreateICmpSGT(result, zero);
    negative = builder_.CreateICmpSLT(result, zero);
  }
  builder_.CreateRet(builder_.CreateSub(builder_.CreateZExt(positive, builder_.getInt32Ty()),
                                        builder_.CreateZExt(negative, builder_.getInt32Ty())));
  function_ = outer;
  builder_.restoreIP(point);
  return fn;
}

llvm::Value *CodeGen::emitValueToString(llvm::Value *value, const Type *type) {
  return callReturningString(rt_.valueToString, {typeDescriptor(type), spill(value, "value")});
}

} // namespace tz
