#include "compiler/lower/objects/dict_method_lowerer/dict_method_lowerer.h"
#include "compiler/lower/errors/may_throw/may_throw.h"
#include "compiler/lower/errors/throw_lowerer/throw_lowerer.h"
#include "compiler/lower/expr/assign_lowerer/assign_lowerer.h"
#include "compiler/lower/expr/expr_lowerer/expr_lowerer.h"
#include "compiler/lower/function_frame/function_frame.h"
#include "compiler/lower/iterators/inline_generator_lowerer/inline_generator_lowerer.h"
#include "compiler/lower/iterators/iterator_lowerer/iterator_lowerer.h"
#include "compiler/lower/iterators/lazy_chain_lowerer/lazy_chain_lowerer.h"
#include "compiler/lower/layouts/object_layouts/object_layouts.h"
#include "compiler/lower/layouts/type_descriptors/type_descriptors.h"
#include "compiler/lower/memory/group_lowerer/group_lowerer.h"
#include "compiler/lower/memory/ref_counts/ref_counts.h"
#include "compiler/lower/memory/store_lowerer/store_lowerer.h"
#include "compiler/lower/module_builder/module_builder.h"
#include "compiler/lower/objects/dict_lowerer/dict_lowerer.h"
#include "compiler/lower/types/optional_lowerer/optional_lowerer.h"
#include "compiler/lower/values/array_lowerer/array_lowerer.h"
#include "compiler/lower/values/string_lowerer/string_lowerer.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/IR/MDBuilder.h"
#include "llvm/Support/ConvertUTF.h"

using llvm::BasicBlock;
using llvm::cast;
using llvm::dyn_cast;
using llvm::isa;
using llvm::Value;

namespace tz {

namespace {

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

}

DictMethodLowerer::DictMethodLowerer(Lowering &lowering) : lowering_(lowering) {}

llvm::Value *DictMethodLowerer::emitDictCall(CallExpr *call) {
  llvm::Type *ptr = lowering_.builder_.getPtrTy(), *i32 = lowering_.builder_.getInt32Ty();
  switch (call->builtin) {
  case Builtin::DictNew:
    return emitDictNew(call);
  case Builtin::DictGroupBy:
    return emitGroupBy(call);
  case Builtin::MapForEach:
    return lowering_.dictLowerer().emitDictForEach(call);
  case Builtin::EnvMap:
    return lowering_.builder_.CreateCall(
        lowering_.dictLowerer().dictFunction("tz_process_env_map", ptr, {ptr}),
        {lowering_.dictLowerer().dictLayout(call->type).descriptor}, "env");
  default:
    break;
  }
  if (isDictIterator(call->builtin)) {
    return lowering_.iteratorLowerer().emitIteratorBuiltin(call);
  }
  Expr *receiver = cast<MemberExpr>(call->callee)->object;
  const Type *type = receiver->type;
  const Type *key = type->keyType(), *value = type->valueType();
  switch (call->builtin) {
  case Builtin::MapSize: {
    Lowering::Operand map = lowering_.refCounts().emitOperand(receiver, true);
    llvm::Value *size = lowering_.builder_.CreateLoad(
        i32, lowering_.dictLowerer().dictField(map.value, kMapCount), "size");
    lowering_.refCounts().dropOperand(map);
    return size;
  }
  case Builtin::MapClear: {
    Lowering::Operand map = lowering_.refCounts().emitOperand(receiver, true);
    lowering_.builder_.CreateCall(
        lowering_.dictLowerer().dictFunction("tz_map_clear", lowering_.builder_.getVoidTy(), {ptr}),
        {map.value});
    lowering_.refCounts().dropOperand(map);
    return nullptr;
  }
  case Builtin::MapGet:
  case Builtin::MapHas:
  case Builtin::MapDelete: {
    Expr *keyExpr = call->args[0];
    Lowering::Operand map =
        lowering_.refCounts().emitOperand(receiver, AssignLowerer::cannotAssign(keyExpr));
    lowering_.throwLowerer().holdOperand(map, lowering_.mayThrow().exprThrows(keyExpr));
    Lowering::Operand k = lowering_.refCounts().emitOperand(keyExpr, true);
    lowering_.throwLowerer().takeOperand(map);
    llvm::Value *result;
    if (call->builtin == Builtin::MapDelete) {
      llvm::Value *removed;
      if (key->isString()) {
        removed = lowering_.builder_.CreateCall(
            lowering_.dictLowerer().dictFunction("tz_map_delete_str", i32,
                                                 {ptr, ptr, lowering_.builder_.getInt64Ty()}),
            {map.value, lowering_.stringLowerer().stringObj(k.value),
             lowering_.stringLowerer().stringMeta(k.value)});
      } else {
        removed = lowering_.builder_.CreateCall(
            lowering_.dictLowerer().dictFunction("tz_map_delete", i32, {ptr, ptr}),
            {map.value, lowering_.functionFrame().spill(k.value, "key")});
      }
      result = lowering_.builder_.CreateICmpNE(removed, lowering_.builder_.getInt32(0));
    } else {
      llvm::Value *entry = lowering_.dictLowerer().emitDictFind(map.value, k.value, key);
      llvm::Value *found = lowering_.builder_.CreateIsNotNull(entry, "found");
      if (call->builtin == Builtin::MapHas) {
        result = found;
      } else {
        result = lowering_.arrayLowerer().emitCheckedRead(found, call->type, [&] {
          llvm::Value *v =
              lowering_.builder_.CreateLoad(lowering_.moduleBuilder().lower(value),
                                            lowering_.dictLowerer().entryValue(entry, type));
          lowering_.refCounts().retainValue(v, value);
          return v;
        });
      }
    }
    lowering_.refCounts().dropOperand(k);
    lowering_.refCounts().dropOperand(map);
    return result;
  }
  case Builtin::MapSet:
  case Builtin::SetAdd:
  case Builtin::RecordSet: {
    Expr *keyExpr = call->args[0];
    Expr *valueExpr = call->args.size() > 1 ? call->args[1] : nullptr;
    bool later = valueExpr && !AssignLowerer::cannotAssign(valueExpr);
    Lowering::Operand map =
        lowering_.refCounts().emitOperand(receiver, AssignLowerer::cannotAssign(keyExpr) && !later);
    lowering_.throwLowerer().holdOperand(
        map, lowering_.mayThrow().exprThrows(keyExpr) ||
                 (valueExpr && lowering_.mayThrow().exprThrows(valueExpr)));
    Lowering::Operand k = lowering_.refCounts().emitOperand(keyExpr, !later);
    lowering_.throwLowerer().holdOperand(k,
                                         valueExpr && lowering_.mayThrow().exprThrows(valueExpr));
    llvm::Value *v = valueExpr ? lowering_.exprLowerer().emitExpr(valueExpr) : nullptr;
    lowering_.throwLowerer().takeOperand(k);
    lowering_.throwLowerer().takeOperand(map);
    llvm::Value *entry = lowering_.dictLowerer().emitDictAdd(map.value, k.value, key);
    if (v) {
      lowering_.storeLowerer().storeIn(map.value, type,
                                       lowering_.dictLowerer().entryValue(entry, type), v, value);
    }
    lowering_.refCounts().dropOperand(k);
    if (call->builtin == Builtin::RecordSet) {
      lowering_.dictLowerer().emitEntryStored(map.value, entry);
      llvm::Value *result = lowering_.builder_.CreateLoad(
          lowering_.moduleBuilder().lower(value), lowering_.dictLowerer().entryValue(entry, type));
      lowering_.refCounts().retainValue(result, value);
      lowering_.refCounts().dropOperand(map);
      return result;
    }
    if (!map.owned) {
      lowering_.refCounts().retainValue(map.value, type);
    }
    return map.value;
  }
  case Builtin::RecordNullishSet: {
    Expr *keyExpr = call->args[0], *valueExpr = call->args[1];
    const Type *resultType = call->type;
    llvm::Value *m = lowering_.exprLowerer().emitExpr(receiver);
    Lowering::Held heldMap = lowering_.throwLowerer().hold(
        m, type,
        lowering_.mayThrow().exprThrows(keyExpr) || lowering_.mayThrow().exprThrows(valueExpr));
    llvm::Value *kv = lowering_.exprLowerer().emitExpr(keyExpr);
    Lowering::Held heldKey =
        lowering_.throwLowerer().hold(kv, key, lowering_.mayThrow().exprThrows(valueExpr));
    llvm::Value *entry = lowering_.dictLowerer().emitDictFind(m, kv, key);
    llvm::BasicBlock *lookBlock = lowering_.functionFrame().newBlock("nullish.look");
    llvm::BasicBlock *keepBlock = lowering_.functionFrame().newBlock("nullish.keep");
    llvm::BasicBlock *assignBlock = lowering_.functionFrame().newBlock("nullish.assign");
    llvm::BasicBlock *endBlock = lowering_.functionFrame().newBlock("nullish.end");
    lowering_.builder_.CreateCondBr(lowering_.builder_.CreateIsNotNull(entry), lookBlock,
                                    assignBlock);
    lowering_.functionFrame().switchTo(lookBlock);
    llvm::Value *old = lowering_.builder_.CreateLoad(
        lowering_.moduleBuilder().lower(value), lowering_.dictLowerer().entryValue(entry, type));
    lowering_.builder_.CreateCondBr(value->isOptional()
                                        ? lowering_.optionalLowerer().optionalHas(old, value)
                                        : lowering_.builder_.getTrue(),
                                    keepBlock, assignBlock);
    lowering_.functionFrame().switchTo(keepBlock);
    lowering_.refCounts().retainValue(old, value);
    llvm::Value *kept = lowering_.exprLowerer().emitConversion(old, value, resultType);
    llvm::BasicBlock *keepEnd = lowering_.builder_.GetInsertBlock();
    lowering_.builder_.CreateBr(endBlock);
    lowering_.functionFrame().switchTo(assignBlock);
    llvm::Value *v = lowering_.exprLowerer().emitExpr(valueExpr);
    llvm::Value *added = lowering_.dictLowerer().emitDictAdd(m, kv, key);
    lowering_.storeLowerer().storeIn(m, type, lowering_.dictLowerer().entryValue(added, type), v,
                                     value);
    lowering_.dictLowerer().emitEntryStored(m, added);
    lowering_.refCounts().retainValue(v, value);
    llvm::Value *assigned = lowering_.exprLowerer().emitConversion(v, value, resultType);
    llvm::BasicBlock *assignEnd = lowering_.builder_.GetInsertBlock();
    lowering_.builder_.CreateBr(endBlock);
    lowering_.functionFrame().switchTo(endBlock);
    llvm::PHINode *result =
        lowering_.builder_.CreatePHI(lowering_.moduleBuilder().lower(resultType), 2);
    result->addIncoming(kept, keepEnd);
    result->addIncoming(assigned, assignEnd);
    lowering_.refCounts().releaseValue(lowering_.throwLowerer().take(heldKey), key);
    lowering_.refCounts().releaseValue(lowering_.throwLowerer().take(heldMap), type);
    return result;
  }
  default: {
    Expr *other = call->args[0];
    Lowering::Operand a =
        lowering_.refCounts().emitOperand(receiver, AssignLowerer::cannotAssign(other));
    lowering_.throwLowerer().holdOperand(a, lowering_.mayThrow().exprThrows(other));
    Lowering::Operand b = lowering_.refCounts().emitOperand(other, true);
    lowering_.throwLowerer().takeOperand(a);
    llvm::Value *result;
    switch (call->builtin) {
    case Builtin::SetUnion:
    case Builtin::SetIntersection:
    case Builtin::SetDifference:
    case Builtin::SetSymmetricDifference: {
      int op = int(call->builtin) - int(Builtin::SetUnion);
      result = lowering_.builder_.CreateCall(
          lowering_.dictLowerer().dictFunction("tz_map_set_op", ptr, {ptr, ptr, i32}),
          {a.value, b.value, lowering_.builder_.getInt32(uint32_t(op))}, "set");
      break;
    }
    case Builtin::SetIsSubsetOf:
    case Builtin::SetIsSupersetOf:
    case Builtin::SetIsDisjointFrom: {
      int op = int(call->builtin) - int(Builtin::SetIsSubsetOf);
      result = lowering_.builder_.CreateICmpNE(
          lowering_.builder_.CreateCall(
              lowering_.dictLowerer().dictFunction("tz_map_set_test", i32, {ptr, ptr, i32}),
              {a.value, b.value, lowering_.builder_.getInt32(uint32_t(op))}),
          lowering_.builder_.getInt32(0));
      break;
    }
    default:
      llvm_unreachable("not a method of a dictionary");
    }
    lowering_.refCounts().dropOperand(b);
    lowering_.refCounts().dropOperand(a);
    return result;
  }
  }
}

llvm::Value *DictMethodLowerer::emitDictNew(CallExpr *call) {
  const Type *type = call->type;
  const Lowering::DictLayout &layout = lowering_.dictLowerer().dictLayout(type);
  llvm::Type *ptr = lowering_.builder_.getPtrTy();
  llvm::FunctionCallee create = lowering_.dictLowerer().dictFunction("tz_map_new", ptr, {ptr});
  llvm::Constant *descriptor =
      lowering_.makesMember(call, type) ? layout.descriptor : layout.plainDescriptor;
  if (call->args.empty()) {
    llvm::Value *map = lowering_.builder_.CreateCall(create, {descriptor}, "map");
    lowering_.groupLowerer().noteUnstored(map, call);
    return map;
  }
  Expr *source = call->args[0];
  if (auto *iterator = dyn_cast<CallExpr>(source);
      iterator &&
      (iterator->builtin == Builtin::MapEntries || iterator->builtin == Builtin::SetValues)) {
    Expr *from = cast<MemberExpr>(iterator->callee)->object;
    if (from->type == type) {
      Lowering::Operand map = lowering_.refCounts().emitOperand(from, true);
      llvm::Value *copy = lowering_.builder_.CreateCall(
          lowering_.dictLowerer().dictFunction("tz_map_copy", ptr, {ptr, ptr}),
          {map.value, descriptor}, "map");
      lowering_.refCounts().dropOperand(map);
      return copy;
    }
  }
  const Type *element = source->type->isArray()    ? source->type->inner()
                        : source->type->isString() ? source->type
                                                   : source->type->yielded();
  lowering_.functionFrame().pushScope();
  Lowering::Producer producer = lowering_.inlineGeneratorLowerer().prepare(source);
  llvm::Value *map = lowering_.builder_.CreateCall(create, {descriptor}, "map");
  lowering_.groupLowerer().noteUnstored(map, call);
  Lowering::Held held = lowering_.throwLowerer().hold(map, type, true);
  size_t depth = lowering_.scopes_.size();
  llvm::BasicBlock *endBlock = lowering_.functionFrame().newBlock("dict.filled");
  const Type *key = type->keyType(), *value = type->valueType();
  Lowering::Consumer consumer;
  consumer.stop = [this, depth, endBlock] {
    lowering_.functionFrame().emitCleanups(depth);
    lowering_.builder_.CreateBr(endBlock);
  };
  consumer.each = [&](llvm::Value *given) -> llvm::Value * {
    if (type->isSet()) {
      llvm::Value *k = lowering_.exprLowerer().emitConversion(given, element, key);
      lowering_.dictLowerer().emitDictAdd(map, k, key);
      lowering_.refCounts().releaseValue(k, key);
      return nullptr;
    }
    const Type *kType = element->fields()[0].type, *vType = element->fields()[1].type;
    llvm::Value *k =
        lowering_.builder_.CreateLoad(lowering_.moduleBuilder().lower(kType),
                                      lowering_.objectLayouts().fieldAddress(given, element, "0"));
    llvm::Value *v =
        lowering_.builder_.CreateLoad(lowering_.moduleBuilder().lower(vType),
                                      lowering_.objectLayouts().fieldAddress(given, element, "1"));
    lowering_.refCounts().retainValue(k, kType);
    lowering_.refCounts().retainValue(v, vType);
    lowering_.refCounts().releaseValue(given, element);
    k = lowering_.exprLowerer().emitConversion(k, kType, key);
    v = lowering_.exprLowerer().emitConversion(v, vType, value);
    llvm::Value *entry = lowering_.dictLowerer().emitDictAdd(map, k, key);
    lowering_.storeLowerer().storeIn(map, type, lowering_.dictLowerer().entryValue(entry, type), v,
                                     value);
    lowering_.refCounts().releaseValue(k, key);

    return nullptr;
  };
  producer.run(consumer);
  lowering_.functionFrame().emitBlock(endBlock);
  llvm::Value *result = lowering_.throwLowerer().take(held);
  lowering_.functionFrame().popScope();

  return result;
}

llvm::Value *DictMethodLowerer::emitGroupBy(CallExpr *call) {
  const Type *type = call->type;
  const Lowering::DictLayout &layout = lowering_.dictLowerer().dictLayout(type);
  llvm::Type *ptr = lowering_.builder_.getPtrTy();
  Expr *source = call->args[0];
  const Type *element = source->type->isArray()    ? source->type->inner()
                        : source->type->isString() ? source->type
                                                   : source->type->yielded();
  const Type *callbackType = call->args[1]->type;
  const Type *key = type->keyType(), *group = type->valueType();
  lowering_.functionFrame().pushScope();
  Lowering::Producer producer = lowering_.inlineGeneratorLowerer().prepare(source);
  llvm::Value *callback = lowering_.functionFrame().createManagedSlot(callbackType, "callback");
  lowering_.builder_.CreateStore(lowering_.exprLowerer().emitExpr(call->args[1]), callback);
  llvm::Value *map = lowering_.builder_.CreateCall(
      lowering_.dictLowerer().dictFunction("tz_map_new", ptr, {ptr}), {layout.descriptor}, "map");
  Lowering::Held held = lowering_.throwLowerer().hold(map, type, true);
  llvm::Type *i32 = lowering_.builder_.getInt32Ty();
  if (type->isRecord()) {
    lowering_.builder_.CreateStore(lowering_.builder_.getInt32(4),
                                   lowering_.dictLowerer().dictField(map, kMapFlags));
  }
  llvm::Value *counter = lowering_.functionFrame().createStackSlot(i32, "counter");
  lowering_.builder_.CreateStore(lowering_.builder_.getInt32(0), counter);
  size_t depth = lowering_.scopes_.size();
  llvm::BasicBlock *endBlock = lowering_.functionFrame().newBlock("group.done");
  Lowering::Consumer consumer;
  consumer.stop = [this, depth, endBlock] {
    lowering_.functionFrame().emitCleanups(depth);
    lowering_.builder_.CreateBr(endBlock);
  };
  consumer.each = [&](llvm::Value *item) -> llvm::Value * {
    llvm::Value *count = lowering_.builder_.CreateLoad(i32, counter);
    lowering_.builder_.CreateStore(
        lowering_.builder_.CreateAdd(count, lowering_.builder_.getInt32(1)), counter);
    lowering_.refCounts().retainValue(item, element);
    Lowering::Held heldItem =
        lowering_.throwLowerer().hold(item, element, lowering_.typeThrows(callbackType));
    llvm::Value *k = lowering_.lazyChainLowerer().callCallback(
        callback, callbackType, {item, count}, {element, Type::intTy()});
    item = lowering_.throwLowerer().take(heldItem);
    llvm::Value *added;
    llvm::Value *entry = lowering_.dictLowerer().emitDictAdd(map, k, key, &added);
    lowering_.refCounts().releaseValue(k, key);
    llvm::Value *slot = lowering_.dictLowerer().entryValue(entry, type);
    llvm::BasicBlock *newBlock_ = lowering_.functionFrame().newBlock("group.new");
    llvm::BasicBlock *pushBlock = lowering_.functionFrame().newBlock("group.push");
    lowering_.builder_.CreateCondBr(added, newBlock_, pushBlock);
    lowering_.functionFrame().switchTo(newBlock_);
    llvm::Value *fresh = lowering_.groupLowerer().newArray(group, lowering_.builder_.getInt64(0),
                                                           lowering_.builder_.getInt64(0));
    lowering_.builder_.CreateStore(fresh, slot);
    if (lowering_.isCyclic(type)) {
      lowering_.storeLowerer().groupStore(map, fresh, group);
    }
    lowering_.builder_.CreateBr(pushBlock);
    lowering_.functionFrame().switchTo(pushBlock);
    lowering_.arrayLowerer().emitPush(lowering_.builder_.CreateLoad(ptr, slot, "group"), {item},
                                      group->inner());

    return nullptr;
  };
  producer.run(consumer);
  lowering_.functionFrame().emitBlock(endBlock);
  llvm::Value *result = lowering_.throwLowerer().take(held);
  lowering_.functionFrame().popScope();

  return result;
}

llvm::Value *DictMethodLowerer::emitDictObjectCall(CallExpr *call) {
  Lowering::Operand map = lowering_.refCounts().emitOperand(call->args[0], true);
  const Type *element = call->type->inner();
  llvm::Type *ptr = lowering_.builder_.getPtrTy();
  llvm::Value *result;
  if (call->builtin == Builtin::ObjectKeys) {
    result = lowering_.builder_.CreateCall(
        lowering_.dictLowerer().dictFunction("tz_map_keys", ptr, {ptr, ptr}),
        {map.value, lowering_.typeDescriptors().typeDescriptor(element)}, "keys");
  } else if (call->builtin == Builtin::ObjectValues) {
    result = lowering_.builder_.CreateCall(
        lowering_.dictLowerer().dictFunction("tz_map_values", ptr, {ptr, ptr}),
        {map.value, lowering_.typeDescriptors().typeDescriptor(element)}, "values");
  } else {
    uint64_t size = lowering_.module_->getDataLayout().getTypeAllocSize(
        lowering_.objectLayouts().objectLayout(element).type);
    result = lowering_.builder_.CreateCall(
        lowering_.dictLowerer().dictFunction("tz_map_entries", ptr,
                                             {ptr, ptr, ptr, lowering_.builder_.getInt64Ty()}),
        {map.value, lowering_.typeDescriptors().typeDescriptor(element),
         lowering_.objectLayouts().layoutDescriptor(element, {0, 1}),
         lowering_.builder_.getInt64(size)},
        "entries");
  }
  lowering_.refCounts().dropOperand(map);

  return result;
}

}
