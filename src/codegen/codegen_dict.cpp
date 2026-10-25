// Dictionaries (docs/objects.md, section 9): Record, Map and Set.
//
// A dictionary is a pointer to a hash table of the runtime (TzMap in
// runtime/tzrt.h, runtime/map.c), which starts as every object does, with
// its count and its layout: retaining and releasing it is as for any object,
// and the drop function of the layout is the runtime's. Its entries lie in
// an array in the order they were added, each { i32 hash, i32 live, key,
// value }, laid out here by the types of the keys and values; the runtime
// gets the offsets in the layout (TzMapLayout), one constant per type.
//
// The runtime finds and adds entries (tz_map_find, tz_map_add, and for a
// string key the variants whose fast path is inlined); the compiled code
// reads and stores values in the entries itself, and walks the entries to
// iterate, re-reading where they are after each step, as the body may add
// some. While an iteration is in progress, the map counts it (`iterating`),
// and its entries do not move: the scope of the iteration ends it however it
// is left.

#include "codegen/codegen.h"

#include "llvm/ADT/STLExtras.h"
#include "llvm/IR/MDBuilder.h"
#include "llvm/Support/ConvertUTF.h"

using llvm::cast;
using llvm::dyn_cast;
using llvm::isa;

namespace tz {

namespace {

// TzMap; must match runtime/tzrt.h.
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
constexpr uint32_t kLayoutDict = 4096; // TZ_LAYOUT_DICT
constexpr uint32_t kMapEnv = 1;        // TZ_MAP_ENV
// The offset of `live` in an entry.
constexpr uint64_t kEntryLive = 4;

std::u16string utf16(llvm::StringRef text) {
  llvm::SmallVector<llvm::UTF16, 32> units;
  llvm::convertUTF8ToUTF16String(text, units);
  return std::u16string(units.begin(), units.end());
}

} // namespace

llvm::FunctionCallee CodeGen::dictFunction(llvm::StringRef name, llvm::Type *ret,
                                           llvm::ArrayRef<llvm::Type *> params) {
  llvm::FunctionCallee callee =
      module_->getOrInsertFunction(name, llvm::FunctionType::get(ret, params, false));
  cast<llvm::Function>(callee.getCallee())->addFnAttr(llvm::Attribute::NoUnwind);
  return callee;
}

const CodeGen::DictLayout &CodeGen::dictLayout(const Type *type) {
  auto [it, inserted] = dictLayouts_.try_emplace(type);
  DictLayout &layout = it->second;
  if (!inserted)
    return layout;
  llvm::Type *ptr = builder_.getPtrTy(), *i32 = builder_.getInt32Ty();
  const Type *key = type->keyType(), *value = type->valueType();
  std::vector<llvm::Type *> elements{i32, i32, lower(key)};
  if (!type->isSet())
    elements.push_back(lower(value));
  layout.entry = llvm::StructType::get(ctx_, elements);
  const llvm::DataLayout &data = module_->getDataLayout();
  const llvm::StructLayout *offsets = data.getStructLayout(layout.entry);
  layout.keyOffset = offsets->getElementOffset(2);
  layout.valueOffset = type->isSet() ? uint64_t(0) : offsets->getElementOffset(3);
  layout.size = data.getTypeAllocSize(layout.entry);

  auto *dropType = llvm::FunctionType::get(builder_.getVoidTy(), {ptr}, false);
  llvm::Constant *drop =
      cast<llvm::Constant>(module_->getOrInsertFunction("tz_map_drop", dropType).getCallee());
  llvm::Constant *null = llvm::ConstantPointerNull::get(ptr);
  // Its number tells it from the other objects of a union (see plainTypeId).
  uint32_t kind = type->isRecord() ? 0 : type->isMap() ? 1 : 2; // TZ_MAP_RECORD...
  if (!mapDescTy_) {
    mapDescTy_ = llvm::StructType::create(ctx_, {layoutDescTy_, ptr, ptr, i32, i32, i32, i32},
                                          "tz.maplayout");
  }
  // TZ_LAYOUT_GROUP: the maps are members of groups (runtime/group.c).
  auto make = [&](bool member) {
    uint32_t flags = kLayoutDict | (member ? 0x80000000u : 0u);
    llvm::Constant *base = llvm::ConstantStruct::get(
        layoutDescTy_, {drop, null, builder_.getInt32(0), builder_.getInt32(flags), null,
                        builder_.getInt32(0), builder_.getInt32(plainTypeId(type))});
    llvm::Constant *init = llvm::ConstantStruct::get(
        mapDescTy_, {base, typeDescriptor(key), type->isSet() ? null : typeDescriptor(value),
                     builder_.getInt32(kind), builder_.getInt32(uint32_t(layout.size)),
                     builder_.getInt32(uint32_t(layout.keyOffset)),
                     builder_.getInt32(uint32_t(layout.valueOffset))});
    auto *global = new llvm::GlobalVariable(*module_, mapDescTy_, /*isConstant=*/true,
                                            llvm::GlobalValue::PrivateLinkage, init,
                                            "tz.layout." + type->name());
    global->setUnnamedAddr(llvm::GlobalValue::UnnamedAddr::Global);
    return global;
  };
  layout.descriptor = make(isCyclic(type));
  layout.plainDescriptor = isCyclic(type) ? make(false) : layout.descriptor;
  return layout;
}

llvm::Value *CodeGen::dictField(llvm::Value *map, unsigned field) {
  if (!mapTy_) {
    llvm::Type *ptr = builder_.getPtrTy(), *i32 = builder_.getInt32Ty();
    mapTy_ = llvm::StructType::create(
        ctx_, {builder_.getInt64Ty(), ptr, ptr, ptr, i32, i32, i32, i32, i32, i32, ptr}, "tz.map");
  }
  return builder_.CreateStructGEP(mapTy_, map, field);
}

llvm::Value *CodeGen::emitDictFind(llvm::Value *map, llvm::Value *key, const Type *keyType) {
  llvm::Type *ptr = builder_.getPtrTy();
  if (keyType->isString())
    return builder_.CreateCall(
        dictFunction("tz_map_find_str", ptr, {ptr, ptr, builder_.getInt64Ty()}),
        {map, stringObj(key), stringMeta(key)}, "entry");
  return builder_.CreateCall(dictFunction("tz_map_find", ptr, {ptr, ptr}), {map, spill(key, "key")},
                             "entry");
}

llvm::Value *CodeGen::emitDictAdd(llvm::Value *map, llvm::Value *key, const Type *keyType,
                                  llvm::Value **added) {
  llvm::Type *ptr = builder_.getPtrTy();
  llvm::Value *flag = createStackSlot(builder_.getInt32Ty(), "added");
  llvm::Value *entry;
  if (keyType->isString())
    entry = builder_.CreateCall(
        dictFunction("tz_map_add_str", ptr, {ptr, ptr, builder_.getInt64Ty(), ptr}),
        {map, stringObj(key), stringMeta(key), flag}, "entry");
  else
    entry = builder_.CreateCall(dictFunction("tz_map_add", ptr, {ptr, ptr, ptr}),
                                {map, spill(key, "key"), flag}, "entry");
  if (added)
    *added = builder_.CreateICmpNE(builder_.CreateLoad(builder_.getInt32Ty(), flag),
                                   builder_.getInt32(0), "added");
  return entry;
}

llvm::Value *CodeGen::entryKey(llvm::Value *entry, const Type *type) {
  return builder_.CreateConstInBoundsGEP1_64(builder_.getInt8Ty(), entry,
                                             dictLayout(type).keyOffset, "key.addr");
}

llvm::Value *CodeGen::entryValue(llvm::Value *entry, const Type *type) {
  return builder_.CreateConstInBoundsGEP1_64(builder_.getInt8Ty(), entry,
                                             dictLayout(type).valueOffset, "value.addr");
}

void CodeGen::emitEntryStored(llvm::Value *map, llvm::Value *entry) {
  llvm::Value *flags = builder_.CreateLoad(builder_.getInt32Ty(), dictField(map, kMapFlags));
  llvm::BasicBlock *envBlock = newBlock("env.set");
  llvm::BasicBlock *doneBlock = newBlock("env.done");
  builder_.CreateCondBr(builder_.CreateICmpNE(builder_.CreateAnd(flags, builder_.getInt32(kMapEnv)),
                                              builder_.getInt32(0)),
                        envBlock, doneBlock, llvm::MDBuilder(ctx_).createUnlikelyBranchWeights());
  switchTo(envBlock);
  llvm::Type *ptr = builder_.getPtrTy();
  builder_.CreateCall(dictFunction("tz_map_stored", builder_.getVoidTy(), {ptr, ptr}),
                      {map, entry});
  builder_.CreateBr(doneBlock);
  switchTo(doneBlock);
}

//===----------------------------------------------------------------------===//
// Iterating
//===----------------------------------------------------------------------===//

void CodeGen::beginDictIteration(llvm::Value *slot) {
  llvm::Value *map = builder_.CreateLoad(builder_.getPtrTy(), slot, "map");
  llvm::Value *count = dictField(map, kMapIterating);
  builder_.CreateStore(
      builder_.CreateAdd(builder_.CreateLoad(builder_.getInt32Ty(), count), builder_.getInt32(1)),
      count);
  scopes_.back().dicts.push_back(slot);
}

void CodeGen::emitDictLoop(llvm::Value *slot, const Type *type, llvm::Value *position,
                           llvm::function_ref<void(llvm::Value *)> each) {
  const DictLayout &layout = dictLayout(type);
  llvm::Type *i32 = builder_.getInt32Ty(), *i64 = builder_.getInt64Ty();
  llvm::Value *indexSlot = position ? position : createStackSlot(i32, "position");
  builder_.CreateStore(builder_.getInt32(0), indexSlot);
  llvm::BasicBlock *headBlock = newBlock("dict.next");
  llvm::BasicBlock *entryBlock = newBlock("dict.entry");
  llvm::BasicBlock *liveBlock = newBlock("dict.live");
  llvm::BasicBlock *endBlock = newBlock("dict.end");
  emitBlock(headBlock);
  // Where the entries are, and how many, may change in the body.
  llvm::Value *map = builder_.CreateLoad(builder_.getPtrTy(), slot, "map");
  llvm::Value *index = builder_.CreateLoad(i32, indexSlot, "position");
  llvm::Value *used = builder_.CreateLoad(i32, dictField(map, kMapUsed), "used");
  builder_.CreateCondBr(builder_.CreateICmpULT(index, used), entryBlock, endBlock);
  switchTo(entryBlock);
  builder_.CreateStore(builder_.CreateAdd(index, builder_.getInt32(1)), indexSlot);
  llvm::Value *entries = builder_.CreateLoad(builder_.getPtrTy(), dictField(map, kMapEntries));
  llvm::Value *entry = builder_.CreateInBoundsGEP(
      builder_.getInt8Ty(), entries,
      builder_.CreateMul(builder_.CreateZExt(index, i64), builder_.getInt64(layout.size)), "entry");
  llvm::Value *live = builder_.CreateLoad(
      i32, builder_.CreateConstInBoundsGEP1_64(builder_.getInt8Ty(), entry, kEntryLive), "live");
  builder_.CreateCondBr(builder_.CreateICmpNE(live, builder_.getInt32(0)), liveBlock, headBlock);
  switchTo(liveBlock);
  each(entry);
  branchTo(headBlock);
  switchTo(endBlock);
}

CodeGen::Producer CodeGen::dictProducer(llvm::Value *slot, const Type *type, Builtin mode,
                                        llvm::Value *position) {
  Producer producer;
  producer.run = [this, slot, type, mode, position](const Consumer &consumer) {
    const Type *key = type->keyType(), *value = type->valueType();
    emitDictLoop(slot, type, position, [&](llvm::Value *entry) {
      auto load = [&](bool isKey) {
        const Type *t = isKey ? key : value;
        llvm::Value *v =
            builder_.CreateLoad(lower(t), isKey ? entryKey(entry, type) : entryValue(entry, type));
        retainValue(v, t);
        return v;
      };
      llvm::Value *given;
      if (mode == Builtin::MapKeys || mode == Builtin::SetValues) {
        given = load(true);
      } else if (mode == Builtin::MapValues) {
        given = load(false);
      } else {
        // A new [key, value] each time, as in JavaScript.
        bool set = mode == Builtin::SetEntries;
        const Type *tuple = Type::tuple({key, set ? key : value});
        llvm::Value *k = load(true), *v = load(/*isKey=*/set);
        given = allocObject(tuple, {0, 1});
        builder_.CreateStore(k, fieldAddress(given, tuple, "0"));
        builder_.CreateStore(v, fieldAddress(given, tuple, "1"));
        adoptFields(given, tuple);
      }
      if (llvm::Value *sent = consumer.each(given))
        releaseValue(sent, consumer.sent);
    });
  };
  return producer;
}

// `m.forEach(f)`: f(value, key, map) for each entry, or f(value, value,
// set), as an iteration, which sees the entries the callback adds.
llvm::Value *CodeGen::emitDictForEach(CallExpr *call) {
  Expr *receiver = cast<MemberExpr>(call->callee)->object;
  const Type *type = receiver->type;
  const Type *callbackType = call->args[0]->type;
  pushScope();
  llvm::Value *slot = createManagedSlot(type, "map");
  builder_.CreateStore(emitExpr(receiver), slot);
  beginDictIteration(slot);
  llvm::Value *callback = createManagedSlot(callbackType, "callback");
  builder_.CreateStore(emitExpr(call->args[0]), callback);
  const Type *key = type->keyType();
  const Type *value = type->isSet() ? key : type->valueType();
  emitDictLoop(slot, type, nullptr, [&](llvm::Value *entry) {
    llvm::Value *v = builder_.CreateLoad(lower(value), type->isSet() ? entryKey(entry, type)
                                                                     : entryValue(entry, type));
    llvm::Value *k = builder_.CreateLoad(lower(key), entryKey(entry, type));
    llvm::Value *map = builder_.CreateLoad(builder_.getPtrTy(), slot);
    retainValue(v, value);
    retainValue(k, key);
    retainValue(map, type);
    if (llvm::Value *result = callCallback(callback, callbackType, {v, k, map}, {value, key, type}))
      releaseValue(result, callbackType->ret());
  });
  popScope();
  return nullptr;
}

//===----------------------------------------------------------------------===//
// Calls
//===----------------------------------------------------------------------===//

llvm::Value *CodeGen::emitDictCall(CallExpr *call) {
  llvm::Type *ptr = builder_.getPtrTy(), *i32 = builder_.getInt32Ty();
  switch (call->builtin) {
  case Builtin::DictNew:
    return emitDictNew(call);
  case Builtin::DictGroupBy:
    return emitGroupBy(call);
  case Builtin::MapForEach:
    return emitDictForEach(call);
  case Builtin::EnvMap:
    return builder_.CreateCall(dictFunction("tz_process_env_map", ptr, {ptr}),
                               {dictLayout(call->type).descriptor}, "env");
  default:
    break;
  }
  if (isDictIterator(call->builtin))
    return emitIteratorBuiltin(call);
  Expr *receiver = cast<MemberExpr>(call->callee)->object;
  const Type *type = receiver->type;
  const Type *key = type->keyType(), *value = type->valueType();
  switch (call->builtin) {
  case Builtin::MapSize: {
    Operand map = emitOperand(receiver, /*canBorrow=*/true);
    llvm::Value *size = builder_.CreateLoad(i32, dictField(map.value, kMapCount), "size");
    dropOperand(map);
    return size;
  }
  case Builtin::MapClear: {
    Operand map = emitOperand(receiver, /*canBorrow=*/true);
    builder_.CreateCall(dictFunction("tz_map_clear", builder_.getVoidTy(), {ptr}), {map.value});
    dropOperand(map);
    return nullptr;
  }
  case Builtin::MapGet:
  case Builtin::MapHas:
  case Builtin::MapDelete: {
    Expr *keyExpr = call->args[0];
    Operand map = emitOperand(receiver, cannotAssign(keyExpr));
    holdOperand(map, exprThrows(keyExpr));
    Operand k = emitOperand(keyExpr, /*canBorrow=*/true);
    takeOperand(map);
    llvm::Value *result;
    if (call->builtin == Builtin::MapDelete) {
      llvm::Value *removed;
      if (key->isString())
        removed = builder_.CreateCall(
            dictFunction("tz_map_delete_str", i32, {ptr, ptr, builder_.getInt64Ty()}),
            {map.value, stringObj(k.value), stringMeta(k.value)});
      else
        removed = builder_.CreateCall(dictFunction("tz_map_delete", i32, {ptr, ptr}),
                                      {map.value, spill(k.value, "key")});
      result = builder_.CreateICmpNE(removed, builder_.getInt32(0));
    } else {
      llvm::Value *entry = emitDictFind(map.value, k.value, key);
      llvm::Value *found = builder_.CreateIsNotNull(entry, "found");
      if (call->builtin == Builtin::MapHas)
        result = found;
      else // read before the map may go
        result = emitCheckedRead(found, call->type, [&] {
          llvm::Value *v = builder_.CreateLoad(lower(value), entryValue(entry, type));
          retainValue(v, value);
          return v;
        });
    }
    dropOperand(k);
    dropOperand(map);
    return result;
  }
  case Builtin::MapSet:
  case Builtin::SetAdd:
  case Builtin::RecordSet: {
    // The map, the key and the value in order, then the store.
    Expr *keyExpr = call->args[0];
    Expr *valueExpr = call->args.size() > 1 ? call->args[1] : nullptr;
    bool later = valueExpr && !cannotAssign(valueExpr);
    Operand map = emitOperand(receiver, cannotAssign(keyExpr) && !later);
    holdOperand(map, exprThrows(keyExpr) || (valueExpr && exprThrows(valueExpr)));
    Operand k = emitOperand(keyExpr, /*canBorrow=*/!later);
    holdOperand(k, valueExpr && exprThrows(valueExpr));
    llvm::Value *v = valueExpr ? emitExpr(valueExpr) : nullptr;
    takeOperand(k);
    takeOperand(map);
    llvm::Value *entry = emitDictAdd(map.value, k.value, key);
    if (v)
      storeIn(map.value, type, entryValue(entry, type), v, value);
    dropOperand(k);
    if (call->builtin == Builtin::RecordSet) {
      emitEntryStored(map.value, entry);
      // The value assigned is the value of the assignment.
      llvm::Value *result = builder_.CreateLoad(lower(value), entryValue(entry, type));
      retainValue(result, value);
      dropOperand(map);
      return result;
    }
    // The map itself, for chaining: `m.set(a, 1).set(b, 2)`.
    if (!map.owned)
      retainValue(map.value, type);
    return map.value;
  }
  case Builtin::RecordNullishSet: {
    // `r[k] ??= v`: v only if r has no value at k.
    Expr *keyExpr = call->args[0], *valueExpr = call->args[1];
    const Type *resultType = call->type;
    // Both owned: v may run any code.
    llvm::Value *m = emitExpr(receiver);
    Held heldMap = hold(m, type, exprThrows(keyExpr) || exprThrows(valueExpr));
    llvm::Value *kv = emitExpr(keyExpr);
    Held heldKey = hold(kv, key, exprThrows(valueExpr));
    llvm::Value *entry = emitDictFind(m, kv, key);
    llvm::BasicBlock *lookBlock = newBlock("nullish.look");
    llvm::BasicBlock *keepBlock = newBlock("nullish.keep");
    llvm::BasicBlock *assignBlock = newBlock("nullish.assign");
    llvm::BasicBlock *endBlock = newBlock("nullish.end");
    builder_.CreateCondBr(builder_.CreateIsNotNull(entry), lookBlock, assignBlock);
    switchTo(lookBlock);
    llvm::Value *old = builder_.CreateLoad(lower(value), entryValue(entry, type));
    builder_.CreateCondBr(value->isOptional() ? optionalHas(old, value) : builder_.getTrue(),
                          keepBlock, assignBlock);
    switchTo(keepBlock);
    retainValue(old, value);
    llvm::Value *kept = emitConversion(old, value, resultType);
    llvm::BasicBlock *keepEnd = builder_.GetInsertBlock();
    builder_.CreateBr(endBlock);
    switchTo(assignBlock);
    llvm::Value *v = emitExpr(valueExpr);
    llvm::Value *added = emitDictAdd(m, kv, key);
    storeIn(m, type, entryValue(added, type), v, value);
    emitEntryStored(m, added);
    retainValue(v, value);
    llvm::Value *assigned = emitConversion(v, value, resultType);
    llvm::BasicBlock *assignEnd = builder_.GetInsertBlock();
    builder_.CreateBr(endBlock);
    switchTo(endBlock);
    llvm::PHINode *result = builder_.CreatePHI(lower(resultType), 2);
    result->addIncoming(kept, keepEnd);
    result->addIncoming(assigned, assignEnd);
    releaseValue(take(heldKey), key);
    releaseValue(take(heldMap), type);
    return result;
  }
  default: {
    // The methods of ES2025 that combine two sets.
    Expr *other = call->args[0];
    Operand a = emitOperand(receiver, cannotAssign(other));
    holdOperand(a, exprThrows(other));
    Operand b = emitOperand(other, /*canBorrow=*/true);
    takeOperand(a);
    llvm::Value *result;
    switch (call->builtin) {
    case Builtin::SetUnion:
    case Builtin::SetIntersection:
    case Builtin::SetDifference:
    case Builtin::SetSymmetricDifference: {
      int op = int(call->builtin) - int(Builtin::SetUnion);
      result = builder_.CreateCall(dictFunction("tz_map_set_op", ptr, {ptr, ptr, i32}),
                                   {a.value, b.value, builder_.getInt32(uint32_t(op))}, "set");
      break;
    }
    case Builtin::SetIsSubsetOf:
    case Builtin::SetIsSupersetOf:
    case Builtin::SetIsDisjointFrom: {
      int op = int(call->builtin) - int(Builtin::SetIsSubsetOf);
      result = builder_.CreateICmpNE(
          builder_.CreateCall(dictFunction("tz_map_set_test", i32, {ptr, ptr, i32}),
                              {a.value, b.value, builder_.getInt32(uint32_t(op))}),
          builder_.getInt32(0));
      break;
    }
    default:
      llvm_unreachable("not a method of a dictionary");
    }
    dropOperand(b);
    dropOperand(a);
    return result;
  }
  }
}

// `new Map(entries)`, `new Set(values)` and Object.fromEntries(entries): the
// map is made first, then filled as the source gives its values, with a
// later value for a key replacing an earlier one.
llvm::Value *CodeGen::emitDictNew(CallExpr *call) {
  const Type *type = call->type;
  const DictLayout &layout = dictLayout(type);
  llvm::Type *ptr = builder_.getPtrTy();
  llvm::FunctionCallee create = dictFunction("tz_map_new", ptr, {ptr});
  llvm::Constant *descriptor =
      makesMember(call, type) ? layout.descriptor : layout.plainDescriptor;
  if (call->args.empty()) {
    llvm::Value *map = builder_.CreateCall(create, {descriptor}, "map");
    noteUnstored(map, call);
    return map;
  }
  Expr *source = call->args[0];
  // `new Map(m)` and `new Set(s)` of the same type copy the entries.
  if (auto *iterator = dyn_cast<CallExpr>(source);
      iterator &&
      (iterator->builtin == Builtin::MapEntries || iterator->builtin == Builtin::SetValues)) {
    Expr *from = cast<MemberExpr>(iterator->callee)->object;
    if (from->type == type) {
      Operand map = emitOperand(from, /*canBorrow=*/true);
      llvm::Value *copy = builder_.CreateCall(dictFunction("tz_map_copy", ptr, {ptr, ptr}),
                                              {map.value, descriptor}, "map");
      dropOperand(map);
      return copy;
    }
  }
  const Type *element = source->type->isArray()    ? source->type->inner()
                        : source->type->isString() ? source->type
                                                   : source->type->yielded();
  pushScope(); // the operands of the iteration
  Producer producer = prepare(source);
  llvm::Value *map = builder_.CreateCall(create, {descriptor}, "map");
  noteUnstored(map, call);
  Held held = hold(map, type, /*needed=*/true);
  size_t depth = scopes_.size();
  llvm::BasicBlock *endBlock = newBlock("dict.filled");
  const Type *key = type->keyType(), *value = type->valueType();
  Consumer consumer;
  consumer.stop = [this, depth, endBlock] {
    emitCleanups(depth);
    builder_.CreateBr(endBlock);
  };
  consumer.each = [&](llvm::Value *given) -> llvm::Value * {
    if (type->isSet()) {
      llvm::Value *k = emitConversion(given, element, key);
      emitDictAdd(map, k, key);
      releaseValue(k, key); // the map keeps a copy
      return nullptr;
    }
    // A [key, value] pair, which goes once its parts are taken.
    const Type *kType = element->fields()[0].type, *vType = element->fields()[1].type;
    llvm::Value *k = builder_.CreateLoad(lower(kType), fieldAddress(given, element, "0"));
    llvm::Value *v = builder_.CreateLoad(lower(vType), fieldAddress(given, element, "1"));
    retainValue(k, kType);
    retainValue(v, vType);
    releaseValue(given, element);
    k = emitConversion(k, kType, key);
    v = emitConversion(v, vType, value);
    llvm::Value *entry = emitDictAdd(map, k, key);
    storeIn(map, type, entryValue(entry, type), v, value);
    releaseValue(k, key);
    return nullptr;
  };
  producer.run(consumer);
  emitBlock(endBlock);
  llvm::Value *result = take(held);
  popScope();
  return result;
}

// Map.groupBy(items, f) and Object.groupBy(items, f): each item goes to the
// array of the key f gives for it, in the order the keys first come.
llvm::Value *CodeGen::emitGroupBy(CallExpr *call) {
  const Type *type = call->type;
  const DictLayout &layout = dictLayout(type);
  llvm::Type *ptr = builder_.getPtrTy();
  Expr *source = call->args[0];
  const Type *element = source->type->isArray()    ? source->type->inner()
                        : source->type->isString() ? source->type
                                                   : source->type->yielded();
  const Type *callbackType = call->args[1]->type;
  const Type *key = type->keyType(), *group = type->valueType();
  pushScope();
  Producer producer = prepare(source);
  llvm::Value *callback = createManagedSlot(callbackType, "callback");
  builder_.CreateStore(emitExpr(call->args[1]), callback);
  llvm::Value *map =
      builder_.CreateCall(dictFunction("tz_map_new", ptr, {ptr}), {layout.descriptor}, "map");
  Held held = hold(map, type, /*needed=*/true);
  llvm::Type *i32 = builder_.getInt32Ty();
  // The object of Object.groupBy has no prototype, and prints so.
  if (type->isRecord())
    builder_.CreateStore(builder_.getInt32(4), dictField(map, kMapFlags)); // TZ_MAP_BARE
  llvm::Value *counter = createStackSlot(i32, "counter");
  builder_.CreateStore(builder_.getInt32(0), counter);
  size_t depth = scopes_.size();
  llvm::BasicBlock *endBlock = newBlock("group.done");
  Consumer consumer;
  consumer.stop = [this, depth, endBlock] {
    emitCleanups(depth);
    builder_.CreateBr(endBlock);
  };
  consumer.each = [&](llvm::Value *item) -> llvm::Value * {
    llvm::Value *count = builder_.CreateLoad(i32, counter);
    builder_.CreateStore(builder_.CreateAdd(count, builder_.getInt32(1)), counter);
    retainValue(item, element); // the callback owns what it gets
    Held heldItem = hold(item, element, typeThrows(callbackType));
    llvm::Value *k = callCallback(callback, callbackType, {item, count}, {element, Type::intTy()});
    item = take(heldItem);
    llvm::Value *added;
    llvm::Value *entry = emitDictAdd(map, k, key, &added);
    releaseValue(k, key);
    llvm::Value *slot = entryValue(entry, type);
    llvm::BasicBlock *newBlock_ = newBlock("group.new");
    llvm::BasicBlock *pushBlock = newBlock("group.push");
    builder_.CreateCondBr(added, newBlock_, pushBlock);
    switchTo(newBlock_);
    llvm::Value *fresh = newArray(group, builder_.getInt64(0), builder_.getInt64(0));
    builder_.CreateStore(fresh, slot);
    if (isCyclic(type))
      groupStore(map, fresh, group);
    builder_.CreateBr(pushBlock);
    switchTo(pushBlock);
    emitPush(builder_.CreateLoad(ptr, slot, "group"), {item}, group->inner());
    return nullptr;
  };
  producer.run(consumer);
  emitBlock(endBlock);
  llvm::Value *result = take(held);
  popScope();
  return result;
}

//===----------------------------------------------------------------------===//
// Records as objects
//===----------------------------------------------------------------------===//

// `{ a: 1, [k]: v, ...r, ...o }` where a Record is expected: the keys in
// order, each later one replacing the value of an earlier one but not its
// place, as in JavaScript.
llvm::Value *CodeGen::emitRecordLiteral(ObjectLitExpr *lit) {
  const Type *type = lit->type;
  const DictLayout &layout = dictLayout(type);
  const Type *value = type->valueType(), *string = Type::string();
  llvm::Type *ptr = builder_.getPtrTy();
  llvm::Value *map = builder_.CreateCall(
      dictFunction("tz_map_new", ptr, {ptr}),
      {makesMember(lit, type) ? layout.descriptor : layout.plainDescriptor}, "record");
  bool throws = llvm::any_of(lit->props, [&](const ObjectLitExpr::Property &p) {
    return exprThrows(p.value) || (p.key && exprThrows(p.key));
  });
  Held held = hold(map, type, throws);
  auto put = [&](llvm::Value *k, llvm::Value *v, const Type *from) {
    llvm::Value *entry = emitDictAdd(map, k, string);
    storeIn(map, type, entryValue(entry, type), emitConversion(v, from, value), value);
  };
  for (const ObjectLitExpr::Property &prop : lit->props) {
    if (!prop.spread) {
      Operand k{stringLiteral(utf16(prop.name)), false, string};
      if (prop.key) {
        k = emitOperand(prop.key, /*canBorrow=*/cannotAssign(prop.value));
        holdOperand(k, exprThrows(prop.value));
      }
      llvm::Value *v = emitExpr(prop.value);
      takeOperand(k);
      put(k.value, v, prop.value->type);
      dropOperand(k);
      continue;
    }
    const Type *source = prop.value->type;
    Operand from = emitOperand(prop.value, /*canBorrow=*/true);
    auto omitted = [&](llvm::Value *k) -> llvm::Value * {
      llvm::Value *skip = builder_.getFalse();
      for (llvm::StringRef name : prop.omit)
        skip = builder_.CreateOr(skip, emitValuesEqual(k, stringLiteral(utf16(name)), string));
      return skip;
    };
    if (source->isRecord()) {
      // Nothing runs while the entries are copied, so the source needs no
      // count of its iteration.
      llvm::Value *slot = spill(from.value, "spread");
      emitDictLoop(slot, source, nullptr, [&](llvm::Value *entry) {
        const Type *sourceValue = source->valueType();
        llvm::Value *k = builder_.CreateLoad(lower(string), entryKey(entry, source));
        llvm::BasicBlock *copyBlock = newBlock("spread.copy");
        llvm::BasicBlock *nextBlock = newBlock("spread.next");
        builder_.CreateCondBr(omitted(k), nextBlock, copyBlock);
        switchTo(copyBlock);
        llvm::Value *v = builder_.CreateLoad(lower(sourceValue), entryValue(entry, source));
        retainValue(v, sourceValue);
        put(k, v, sourceValue);
        builder_.CreateBr(nextBlock);
        switchTo(nextBlock);
      });
    } else {
      // The fields that hold a value, in the order of the type.
      for (const Field &f : source->fields()) {
        if (llvm::StringRef(f.name).starts_with("#") || llvm::is_contained(prop.omit, f.name))
          continue;
        llvm::Value *v =
            builder_.CreateLoad(lower(f.type), fieldAddress(from.value, source, f.name));
        llvm::Value *k = stringLiteral(utf16(f.name));
        if (!f.type->isOptional() || value->isOptional()) {
          retainValue(v, f.type);
          put(k, v, f.type);
          continue;
        }
        llvm::BasicBlock *copyBlock = newBlock("spread.copy");
        llvm::BasicBlock *nextBlock = newBlock("spread.next");
        builder_.CreateCondBr(optionalHas(v, f.type), copyBlock, nextBlock);
        switchTo(copyBlock);
        llvm::Value *present = optionalValue(v, f.type);
        retainValue(present, f.type->inner());
        put(k, present, f.type->inner());
        builder_.CreateBr(nextBlock);
        switchTo(nextBlock);
      }
    }
    dropOperand(from);
  }
  return take(held);
}

// Object.keys, Object.values and Object.entries of a Record.
llvm::Value *CodeGen::emitDictObjectCall(CallExpr *call) {
  Operand map = emitOperand(call->args[0], /*canBorrow=*/true);
  const Type *element = call->type->inner();
  llvm::Type *ptr = builder_.getPtrTy();
  llvm::Value *result;
  if (call->builtin == Builtin::ObjectKeys) {
    result = builder_.CreateCall(dictFunction("tz_map_keys", ptr, {ptr, ptr}),
                                 {map.value, typeDescriptor(element)}, "keys");
  } else if (call->builtin == Builtin::ObjectValues) {
    result = builder_.CreateCall(dictFunction("tz_map_values", ptr, {ptr, ptr}),
                                 {map.value, typeDescriptor(element)}, "values");
  } else {
    uint64_t size = module_->getDataLayout().getTypeAllocSize(objectLayout(element).type);
    result = builder_.CreateCall(
        dictFunction("tz_map_entries", ptr, {ptr, ptr, ptr, builder_.getInt64Ty()}),
        {map.value, typeDescriptor(element), layoutDescriptor(element, {0, 1}),
         builder_.getInt64(size)},
        "entries");
  }
  dropOperand(map);
  return result;
}

} // namespace tz
