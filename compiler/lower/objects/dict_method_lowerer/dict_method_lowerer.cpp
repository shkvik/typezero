#include "compiler/lower/objects/dict_method_lowerer/dict_method_lowerer.h"
#include "compiler/lower/module_builder/module_builder.h"
#include "compiler/lower/function_frame/function_frame.h"
#include "compiler/lower/memory/ref_counts/ref_counts.h"
#include "compiler/lower/types/optional_lowerer/optional_lowerer.h"
#include "compiler/lower/expr/expr_lowerer/expr_lowerer.h"
#include "compiler/lower/expr/assign_lowerer/assign_lowerer.h"
#include "compiler/lower/values/string_lowerer/string_lowerer.h"
#include "compiler/lower/layouts/type_descriptors/type_descriptors.h"
#include "compiler/lower/values/array_lowerer/array_lowerer.h"
#include "compiler/lower/memory/group_lowerer/group_lowerer.h"
#include "compiler/lower/memory/store_lowerer/store_lowerer.h"
#include "compiler/lower/objects/dict_lowerer/dict_lowerer.h"
#include "compiler/lower/iterators/lazy_chain_lowerer/lazy_chain_lowerer.h"
#include "compiler/lower/iterators/inline_generator_lowerer/inline_generator_lowerer.h"
#include "compiler/lower/iterators/iterator_lowerer/iterator_lowerer.h"
#include "compiler/lower/layouts/object_layouts/object_layouts.h"
#include "compiler/lower/errors/may_throw/may_throw.h"
#include "compiler/lower/errors/throw_lowerer/throw_lowerer.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/IR/MDBuilder.h"
#include "llvm/Support/ConvertUTF.h"

using llvm::cast;
using llvm::dyn_cast;
using llvm::isa;
using llvm::BasicBlock;
using llvm::Value;

namespace tz {

namespace {

// TzMap; must match runtime/core/tzrt/tzrt.h.
enum MapField : unsigned {
  kMapRefCount,
  kMapLayout,
  kMapEntries,
  kMapIndex,
  kMapCount,
  kMapUsed,
  kMapCapacity,
  kMapIndexMask,
  kMapIterating,
  kMapFlags,
  kMapNextDead,
};

} // namespace

DictMethodLowerer::DictMethodLowerer(CodeGen &codegen) : codegen_(codegen) {}

llvm::Value *DictMethodLowerer::emitDictCall(CallExpr *call) {
  llvm::Type *ptr = codegen_.builder_.getPtrTy(), *i32 = codegen_.builder_.getInt32Ty();
  switch (call->builtin) {
  case Builtin::DictNew:
    return emitDictNew(call);
  case Builtin::DictGroupBy:
    return emitGroupBy(call);
  case Builtin::MapForEach:
    return codegen_.dictLowerer().emitDictForEach(call);
  case Builtin::EnvMap:
    return codegen_.builder_.CreateCall(codegen_.dictLowerer().dictFunction("tz_process_env_map", ptr, {ptr}),
                               {codegen_.dictLowerer().dictLayout(call->type).descriptor}, "env");
  default:
    break;
  }
  if (isDictIterator(call->builtin)) {
    return codegen_.iteratorLowerer().emitIteratorBuiltin(call);
  }
  Expr *receiver = cast<MemberExpr>(call->callee)->object;
  const Type *type = receiver->type;
  const Type *key = type->keyType(), *value = type->valueType();
  switch (call->builtin) {
  case Builtin::MapSize: {
    CodeGen::Operand map = codegen_.refCounts().emitOperand(receiver, /*canBorrow=*/true);
    llvm::Value *size = codegen_.builder_.CreateLoad(i32, codegen_.dictLowerer().dictField(map.value, kMapCount), "size");
    codegen_.refCounts().dropOperand(map);
    return size;
  }
  case Builtin::MapClear: {
    CodeGen::Operand map = codegen_.refCounts().emitOperand(receiver, /*canBorrow=*/true);
    codegen_.builder_.CreateCall(codegen_.dictLowerer().dictFunction("tz_map_clear", codegen_.builder_.getVoidTy(), {ptr}), {map.value});
    codegen_.refCounts().dropOperand(map);
    return nullptr;
  }
  case Builtin::MapGet:
  case Builtin::MapHas:
  case Builtin::MapDelete: {
    Expr *keyExpr = call->args[0];
    CodeGen::Operand map = codegen_.refCounts().emitOperand(receiver, AssignLowerer::cannotAssign(keyExpr));
    codegen_.throwLowerer().holdOperand(map, codegen_.mayThrow().exprThrows(keyExpr));
    CodeGen::Operand k = codegen_.refCounts().emitOperand(keyExpr, /*canBorrow=*/true);
    codegen_.throwLowerer().takeOperand(map);
    llvm::Value *result;
    if (call->builtin == Builtin::MapDelete) {
      llvm::Value *removed;
      if (key->isString()) {
        removed = codegen_.builder_.CreateCall(
            codegen_.dictLowerer().dictFunction("tz_map_delete_str", i32, {ptr, ptr, codegen_.builder_.getInt64Ty()}),
            {map.value, codegen_.stringLowerer().stringObj(k.value), codegen_.stringLowerer().stringMeta(k.value)});
      } else {
        removed = codegen_.builder_.CreateCall(codegen_.dictLowerer().dictFunction("tz_map_delete", i32, {ptr, ptr}),
                                      {map.value, codegen_.functionFrame().spill(k.value, "key")});
      }
      result = codegen_.builder_.CreateICmpNE(removed, codegen_.builder_.getInt32(0));
    } else {
      llvm::Value *entry = codegen_.dictLowerer().emitDictFind(map.value, k.value, key);
      llvm::Value *found = codegen_.builder_.CreateIsNotNull(entry, "found");
      if (call->builtin == Builtin::MapHas) {
        result = found;
      } else { // read before the map may go
        result = codegen_.arrayLowerer().emitCheckedRead(found, call->type, [&] {
          llvm::Value *v = codegen_.builder_.CreateLoad(codegen_.moduleBuilder().lower(value), codegen_.dictLowerer().entryValue(entry, type));
          codegen_.refCounts().retainValue(v, value);
          return v;
        });
      }
    }
    codegen_.refCounts().dropOperand(k);
    codegen_.refCounts().dropOperand(map);
    return result;
  }
  case Builtin::MapSet:
  case Builtin::SetAdd:
  case Builtin::RecordSet: {
    // The map, the key and the value in order, then the store.
    Expr *keyExpr = call->args[0];
    Expr *valueExpr = call->args.size() > 1 ? call->args[1] : nullptr;
    bool later = valueExpr && !AssignLowerer::cannotAssign(valueExpr);
    CodeGen::Operand map = codegen_.refCounts().emitOperand(receiver, AssignLowerer::cannotAssign(keyExpr) && !later);
    codegen_.throwLowerer().holdOperand(map, codegen_.mayThrow().exprThrows(keyExpr) || (valueExpr && codegen_.mayThrow().exprThrows(valueExpr)));
    CodeGen::Operand k = codegen_.refCounts().emitOperand(keyExpr, /*canBorrow=*/!later);
    codegen_.throwLowerer().holdOperand(k, valueExpr && codegen_.mayThrow().exprThrows(valueExpr));
    llvm::Value *v = valueExpr ? codegen_.exprLowerer().emitExpr(valueExpr) : nullptr;
    codegen_.throwLowerer().takeOperand(k);
    codegen_.throwLowerer().takeOperand(map);
    llvm::Value *entry = codegen_.dictLowerer().emitDictAdd(map.value, k.value, key);
    if (v) {
      codegen_.storeLowerer().storeIn(map.value, type, codegen_.dictLowerer().entryValue(entry, type), v, value);
    }
    codegen_.refCounts().dropOperand(k);
    if (call->builtin == Builtin::RecordSet) {
      codegen_.dictLowerer().emitEntryStored(map.value, entry);
      // The value assigned is the value of the assignment.
      llvm::Value *result = codegen_.builder_.CreateLoad(codegen_.moduleBuilder().lower(value), codegen_.dictLowerer().entryValue(entry, type));
      codegen_.refCounts().retainValue(result, value);
      codegen_.refCounts().dropOperand(map);
      return result;
    }
    // The map itself, for chaining: `m.set(a, 1).set(b, 2)`.
    if (!map.owned) {
      codegen_.refCounts().retainValue(map.value, type);
    }
    return map.value;
  }
  case Builtin::RecordNullishSet: {
    // `r[k] ??= v`: v only if r has no value at k.
    Expr *keyExpr = call->args[0], *valueExpr = call->args[1];
    const Type *resultType = call->type;
    // Both owned: v may run any code.
    llvm::Value *m = codegen_.exprLowerer().emitExpr(receiver);
    CodeGen::Held heldMap = codegen_.throwLowerer().hold(m, type, codegen_.mayThrow().exprThrows(keyExpr) || codegen_.mayThrow().exprThrows(valueExpr));
    llvm::Value *kv = codegen_.exprLowerer().emitExpr(keyExpr);
    CodeGen::Held heldKey = codegen_.throwLowerer().hold(kv, key, codegen_.mayThrow().exprThrows(valueExpr));
    llvm::Value *entry = codegen_.dictLowerer().emitDictFind(m, kv, key);
    llvm::BasicBlock *lookBlock = codegen_.functionFrame().newBlock("nullish.look");
    llvm::BasicBlock *keepBlock = codegen_.functionFrame().newBlock("nullish.keep");
    llvm::BasicBlock *assignBlock = codegen_.functionFrame().newBlock("nullish.assign");
    llvm::BasicBlock *endBlock = codegen_.functionFrame().newBlock("nullish.end");
    codegen_.builder_.CreateCondBr(codegen_.builder_.CreateIsNotNull(entry), lookBlock, assignBlock);
    codegen_.functionFrame().switchTo(lookBlock);
    llvm::Value *old = codegen_.builder_.CreateLoad(codegen_.moduleBuilder().lower(value), codegen_.dictLowerer().entryValue(entry, type));
    codegen_.builder_.CreateCondBr(value->isOptional() ? codegen_.optionalLowerer().optionalHas(old, value) : codegen_.builder_.getTrue(),
                          keepBlock, assignBlock);
    codegen_.functionFrame().switchTo(keepBlock);
    codegen_.refCounts().retainValue(old, value);
    llvm::Value *kept = codegen_.exprLowerer().emitConversion(old, value, resultType);
    llvm::BasicBlock *keepEnd = codegen_.builder_.GetInsertBlock();
    codegen_.builder_.CreateBr(endBlock);
    codegen_.functionFrame().switchTo(assignBlock);
    llvm::Value *v = codegen_.exprLowerer().emitExpr(valueExpr);
    llvm::Value *added = codegen_.dictLowerer().emitDictAdd(m, kv, key);
    codegen_.storeLowerer().storeIn(m, type, codegen_.dictLowerer().entryValue(added, type), v, value);
    codegen_.dictLowerer().emitEntryStored(m, added);
    codegen_.refCounts().retainValue(v, value);
    llvm::Value *assigned = codegen_.exprLowerer().emitConversion(v, value, resultType);
    llvm::BasicBlock *assignEnd = codegen_.builder_.GetInsertBlock();
    codegen_.builder_.CreateBr(endBlock);
    codegen_.functionFrame().switchTo(endBlock);
    llvm::PHINode *result = codegen_.builder_.CreatePHI(codegen_.moduleBuilder().lower(resultType), 2);
    result->addIncoming(kept, keepEnd);
    result->addIncoming(assigned, assignEnd);
    codegen_.refCounts().releaseValue(codegen_.throwLowerer().take(heldKey), key);
    codegen_.refCounts().releaseValue(codegen_.throwLowerer().take(heldMap), type);
    return result;
  }
  default: {
    // The methods of ES2025 that combine two sets.
    Expr *other = call->args[0];
    CodeGen::Operand a = codegen_.refCounts().emitOperand(receiver, AssignLowerer::cannotAssign(other));
    codegen_.throwLowerer().holdOperand(a, codegen_.mayThrow().exprThrows(other));
    CodeGen::Operand b = codegen_.refCounts().emitOperand(other, /*canBorrow=*/true);
    codegen_.throwLowerer().takeOperand(a);
    llvm::Value *result;
    switch (call->builtin) {
    case Builtin::SetUnion:
    case Builtin::SetIntersection:
    case Builtin::SetDifference:
    case Builtin::SetSymmetricDifference: {
      int op = int(call->builtin) - int(Builtin::SetUnion);
      result = codegen_.builder_.CreateCall(codegen_.dictLowerer().dictFunction("tz_map_set_op", ptr, {ptr, ptr, i32}),
                                   {a.value, b.value, codegen_.builder_.getInt32(uint32_t(op))}, "set");
      break;
    }
    case Builtin::SetIsSubsetOf:
    case Builtin::SetIsSupersetOf:
    case Builtin::SetIsDisjointFrom: {
      int op = int(call->builtin) - int(Builtin::SetIsSubsetOf);
      result = codegen_.builder_.CreateICmpNE(
          codegen_.builder_.CreateCall(codegen_.dictLowerer().dictFunction("tz_map_set_test", i32, {ptr, ptr, i32}),
                              {a.value, b.value, codegen_.builder_.getInt32(uint32_t(op))}),
          codegen_.builder_.getInt32(0));
      break;
    }
    default:
      llvm_unreachable("not a method of a dictionary");
    }
    codegen_.refCounts().dropOperand(b);
    codegen_.refCounts().dropOperand(a);
    return result;
  }
  }
}

// `new Map(entries)`, `new Set(values)` and Object.fromEntries(entries): the
// map is made first, then filled as the source gives its values, with a
// later value for a key replacing an earlier one.
llvm::Value *DictMethodLowerer::emitDictNew(CallExpr *call) {
  const Type *type = call->type;
  const CodeGen::DictLayout &layout = codegen_.dictLowerer().dictLayout(type);
  llvm::Type *ptr = codegen_.builder_.getPtrTy();
  llvm::FunctionCallee create = codegen_.dictLowerer().dictFunction("tz_map_new", ptr, {ptr});
  llvm::Constant *descriptor = codegen_.makesMember(call, type) ? layout.descriptor : layout.plainDescriptor;
  if (call->args.empty()) {
    llvm::Value *map = codegen_.builder_.CreateCall(create, {descriptor}, "map");
    codegen_.groupLowerer().noteUnstored(map, call);
    return map;
  }
  Expr *source = call->args[0];
  // `new Map(m)` and `new Set(s)` of the same type copy the entries.
  if (auto *iterator = dyn_cast<CallExpr>(source);
      iterator &&
      (iterator->builtin == Builtin::MapEntries || iterator->builtin == Builtin::SetValues)) {
    Expr *from = cast<MemberExpr>(iterator->callee)->object;
    if (from->type == type) {
      CodeGen::Operand map = codegen_.refCounts().emitOperand(from, /*canBorrow=*/true);
      llvm::Value *copy = codegen_.builder_.CreateCall(codegen_.dictLowerer().dictFunction("tz_map_copy", ptr, {ptr, ptr}),
                                              {map.value, descriptor}, "map");
      codegen_.refCounts().dropOperand(map);
      return copy;
    }
  }
  const Type *element = source->type->isArray()    ? source->type->inner()
                        : source->type->isString() ? source->type
                                                   : source->type->yielded();
  codegen_.functionFrame().pushScope(); // the operands of the iteration
  CodeGen::Producer producer = codegen_.inlineGeneratorLowerer().prepare(source);
  llvm::Value *map = codegen_.builder_.CreateCall(create, {descriptor}, "map");
  codegen_.groupLowerer().noteUnstored(map, call);
  CodeGen::Held held = codegen_.throwLowerer().hold(map, type, /*needed=*/true);
  size_t depth = codegen_.scopes_.size();
  llvm::BasicBlock *endBlock = codegen_.functionFrame().newBlock("dict.filled");
  const Type *key = type->keyType(), *value = type->valueType();
  CodeGen::Consumer consumer;
  consumer.stop = [this, depth, endBlock] {
    codegen_.functionFrame().emitCleanups(depth);
    codegen_.builder_.CreateBr(endBlock);
  };
  consumer.each = [&](llvm::Value *given) -> llvm::Value * {
    if (type->isSet()) {
      llvm::Value *k = codegen_.exprLowerer().emitConversion(given, element, key);
      codegen_.dictLowerer().emitDictAdd(map, k, key);
      codegen_.refCounts().releaseValue(k, key); // the map keeps a copy
      return nullptr;
    }
    // A [key, value] pair, which goes once its parts are taken.
    const Type *kType = element->fields()[0].type, *vType = element->fields()[1].type;
    llvm::Value *k = codegen_.builder_.CreateLoad(codegen_.moduleBuilder().lower(kType), codegen_.objectLayouts().fieldAddress(given, element, "0"));
    llvm::Value *v = codegen_.builder_.CreateLoad(codegen_.moduleBuilder().lower(vType), codegen_.objectLayouts().fieldAddress(given, element, "1"));
    codegen_.refCounts().retainValue(k, kType);
    codegen_.refCounts().retainValue(v, vType);
    codegen_.refCounts().releaseValue(given, element);
    k = codegen_.exprLowerer().emitConversion(k, kType, key);
    v = codegen_.exprLowerer().emitConversion(v, vType, value);
    llvm::Value *entry = codegen_.dictLowerer().emitDictAdd(map, k, key);
    codegen_.storeLowerer().storeIn(map, type, codegen_.dictLowerer().entryValue(entry, type), v, value);
    codegen_.refCounts().releaseValue(k, key);

    return nullptr;
  };
  producer.run(consumer);
  codegen_.functionFrame().emitBlock(endBlock);
  llvm::Value *result = codegen_.throwLowerer().take(held);
  codegen_.functionFrame().popScope();

  return result;
}

// Map.groupBy(items, f) and Object.groupBy(items, f): each item goes to the
// array of the key f gives for it, in the order the keys first come.
llvm::Value *DictMethodLowerer::emitGroupBy(CallExpr *call) {
  const Type *type = call->type;
  const CodeGen::DictLayout &layout = codegen_.dictLowerer().dictLayout(type);
  llvm::Type *ptr = codegen_.builder_.getPtrTy();
  Expr *source = call->args[0];
  const Type *element = source->type->isArray()    ? source->type->inner()
                        : source->type->isString() ? source->type
                                                   : source->type->yielded();
  const Type *callbackType = call->args[1]->type;
  const Type *key = type->keyType(), *group = type->valueType();
  codegen_.functionFrame().pushScope();
  CodeGen::Producer producer = codegen_.inlineGeneratorLowerer().prepare(source);
  llvm::Value *callback = codegen_.functionFrame().createManagedSlot(callbackType, "callback");
  codegen_.builder_.CreateStore(codegen_.exprLowerer().emitExpr(call->args[1]), callback);
  llvm::Value *map =
      codegen_.builder_.CreateCall(codegen_.dictLowerer().dictFunction("tz_map_new", ptr, {ptr}), {layout.descriptor}, "map");
  CodeGen::Held held = codegen_.throwLowerer().hold(map, type, /*needed=*/true);
  llvm::Type *i32 = codegen_.builder_.getInt32Ty();
  // The object of Object.groupBy has no prototype, and prints so.
  if (type->isRecord()) {
    codegen_.builder_.CreateStore(codegen_.builder_.getInt32(4), codegen_.dictLowerer().dictField(map, kMapFlags)); // TZ_MAP_BARE
  }
  llvm::Value *counter = codegen_.functionFrame().createStackSlot(i32, "counter");
  codegen_.builder_.CreateStore(codegen_.builder_.getInt32(0), counter);
  size_t depth = codegen_.scopes_.size();
  llvm::BasicBlock *endBlock = codegen_.functionFrame().newBlock("group.done");
  CodeGen::Consumer consumer;
  consumer.stop = [this, depth, endBlock] {
    codegen_.functionFrame().emitCleanups(depth);
    codegen_.builder_.CreateBr(endBlock);
  };
  consumer.each = [&](llvm::Value *item) -> llvm::Value * {
    llvm::Value *count = codegen_.builder_.CreateLoad(i32, counter);
    codegen_.builder_.CreateStore(codegen_.builder_.CreateAdd(count, codegen_.builder_.getInt32(1)), counter);
    codegen_.refCounts().retainValue(item, element); // the callback owns what it gets
    CodeGen::Held heldItem = codegen_.throwLowerer().hold(item, element, codegen_.typeThrows(callbackType));
    llvm::Value *k = codegen_.lazyChainLowerer().callCallback(callback, callbackType, {item, count}, {element, Type::intTy()});
    item = codegen_.throwLowerer().take(heldItem);
    llvm::Value *added;
    llvm::Value *entry = codegen_.dictLowerer().emitDictAdd(map, k, key, &added);
    codegen_.refCounts().releaseValue(k, key);
    llvm::Value *slot = codegen_.dictLowerer().entryValue(entry, type);
    llvm::BasicBlock *newBlock_ = codegen_.functionFrame().newBlock("group.new");
    llvm::BasicBlock *pushBlock = codegen_.functionFrame().newBlock("group.push");
    codegen_.builder_.CreateCondBr(added, newBlock_, pushBlock);
    codegen_.functionFrame().switchTo(newBlock_);
    llvm::Value *fresh = codegen_.groupLowerer().newArray(group, codegen_.builder_.getInt64(0), codegen_.builder_.getInt64(0));
    codegen_.builder_.CreateStore(fresh, slot);
    if (codegen_.isCyclic(type)) {
      codegen_.storeLowerer().groupStore(map, fresh, group);
    }
    codegen_.builder_.CreateBr(pushBlock);
    codegen_.functionFrame().switchTo(pushBlock);
    codegen_.arrayLowerer().emitPush(codegen_.builder_.CreateLoad(ptr, slot, "group"), {item}, group->inner());

    return nullptr;
  };
  producer.run(consumer);
  codegen_.functionFrame().emitBlock(endBlock);
  llvm::Value *result = codegen_.throwLowerer().take(held);
  codegen_.functionFrame().popScope();

  return result;
}

// Object.keys, Object.values and Object.entries of a Record.
llvm::Value *DictMethodLowerer::emitDictObjectCall(CallExpr *call) {
  CodeGen::Operand map = codegen_.refCounts().emitOperand(call->args[0], /*canBorrow=*/true);
  const Type *element = call->type->inner();
  llvm::Type *ptr = codegen_.builder_.getPtrTy();
  llvm::Value *result;
  if (call->builtin == Builtin::ObjectKeys) {
    result = codegen_.builder_.CreateCall(codegen_.dictLowerer().dictFunction("tz_map_keys", ptr, {ptr, ptr}),
                                 {map.value, codegen_.typeDescriptors().typeDescriptor(element)}, "keys");
  } else if (call->builtin == Builtin::ObjectValues) {
    result = codegen_.builder_.CreateCall(codegen_.dictLowerer().dictFunction("tz_map_values", ptr, {ptr, ptr}),
                                 {map.value, codegen_.typeDescriptors().typeDescriptor(element)}, "values");
  } else {
    uint64_t size = codegen_.module_->getDataLayout().getTypeAllocSize(codegen_.objectLayouts().objectLayout(element).type);
    result = codegen_.builder_.CreateCall(
        codegen_.dictLowerer().dictFunction("tz_map_entries", ptr, {ptr, ptr, ptr, codegen_.builder_.getInt64Ty()}),
        {map.value, codegen_.typeDescriptors().typeDescriptor(element), codegen_.objectLayouts().layoutDescriptor(element, {0, 1}),
         codegen_.builder_.getInt64(size)},
        "entries");
  }
  codegen_.refCounts().dropOperand(map);

  return result;
}

} // namespace tz
