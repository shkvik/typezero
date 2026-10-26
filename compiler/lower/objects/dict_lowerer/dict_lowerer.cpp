#include "compiler/lower/objects/dict_lowerer/dict_lowerer.h"
#include "compiler/lower/errors/may_throw/may_throw.h"
#include "compiler/lower/errors/throw_lowerer/throw_lowerer.h"
#include "compiler/lower/expr/assign_lowerer/assign_lowerer.h"
#include "compiler/lower/expr/equality_lowerer/equality_lowerer.h"
#include "compiler/lower/expr/expr_lowerer/expr_lowerer.h"
#include "compiler/lower/function_frame/function_frame.h"
#include "compiler/lower/iterators/lazy_chain_lowerer/lazy_chain_lowerer.h"
#include "compiler/lower/layouts/object_layouts/object_layouts.h"
#include "compiler/lower/layouts/type_descriptors/type_descriptors.h"
#include "compiler/lower/memory/group_lowerer/group_lowerer.h"
#include "compiler/lower/memory/ref_counts/ref_counts.h"
#include "compiler/lower/memory/store_lowerer/store_lowerer.h"
#include "compiler/lower/module_builder/module_builder.h"
#include "compiler/lower/types/optional_lowerer/optional_lowerer.h"
#include "compiler/lower/types/union_lowerer/union_lowerer.h"
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

constexpr uint32_t kLayoutDict = 4096;

constexpr uint32_t kMapEnv = 1;

constexpr uint64_t kEntryLive = 4;

std::u16string utf16(llvm::StringRef text) {
  llvm::SmallVector<llvm::UTF16, 32> units;
  llvm::convertUTF8ToUTF16String(text, units);

  return std::u16string(units.begin(), units.end());
}

}

DictLowerer::DictLowerer(Lowering &lowering) : lowering_(lowering) {}

llvm::FunctionCallee DictLowerer::dictFunction(llvm::StringRef name, llvm::Type *ret,
                                               llvm::ArrayRef<llvm::Type *> params) {
  llvm::FunctionCallee callee =
      lowering_.module_->getOrInsertFunction(name, llvm::FunctionType::get(ret, params, false));
  cast<llvm::Function>(callee.getCallee())->addFnAttr(llvm::Attribute::NoUnwind);

  return callee;
}

const Lowering::DictLayout &DictLowerer::dictLayout(const Type *type) {
  auto [it, inserted] = dictLayouts_.try_emplace(type);
  Lowering::DictLayout &layout = it->second;
  if (!inserted) {
    return layout;
  }
  llvm::Type *ptr = lowering_.builder_.getPtrTy(), *i32 = lowering_.builder_.getInt32Ty();
  const Type *key = type->keyType(), *value = type->valueType();
  std::vector<llvm::Type *> elements{i32, i32, lowering_.moduleBuilder().lower(key)};
  if (!type->isSet()) {
    elements.push_back(lowering_.moduleBuilder().lower(value));
  }
  layout.entry = llvm::StructType::get(lowering_.ctx_, elements);
  const llvm::DataLayout &data = lowering_.module_->getDataLayout();
  const llvm::StructLayout *offsets = data.getStructLayout(layout.entry);
  layout.keyOffset = offsets->getElementOffset(2);
  layout.valueOffset = type->isSet() ? uint64_t(0) : offsets->getElementOffset(3);
  layout.size = data.getTypeAllocSize(layout.entry);

  auto *dropType = llvm::FunctionType::get(lowering_.builder_.getVoidTy(), {ptr}, false);
  llvm::Constant *drop = cast<llvm::Constant>(
      lowering_.module_->getOrInsertFunction("tz_map_drop", dropType).getCallee());
  llvm::Constant *null = llvm::ConstantPointerNull::get(ptr);
  uint32_t kind = type->isRecord() ? 0 : type->isMap() ? 1 : 2;
  if (!mapDescTy_) {
    mapDescTy_ = llvm::StructType::create(
        lowering_.ctx_, {lowering_.layoutDescTy_, ptr, ptr, i32, i32, i32, i32}, "tz.maplayout");
  }
  auto make = [&](bool member) {
    uint32_t flags = kLayoutDict | (member ? 0x80000000u : 0u);
    llvm::Constant *base = llvm::ConstantStruct::get(
        lowering_.layoutDescTy_,
        {drop, null, lowering_.builder_.getInt32(0), lowering_.builder_.getInt32(flags), null,
         lowering_.builder_.getInt32(0),
         lowering_.builder_.getInt32(lowering_.unionLowerer().plainTypeId(type))});
    llvm::Constant *init = llvm::ConstantStruct::get(
        mapDescTy_,
        {base, lowering_.typeDescriptors().typeDescriptor(key),
         type->isSet() ? null : lowering_.typeDescriptors().typeDescriptor(value),
         lowering_.builder_.getInt32(kind), lowering_.builder_.getInt32(uint32_t(layout.size)),
         lowering_.builder_.getInt32(uint32_t(layout.keyOffset)),
         lowering_.builder_.getInt32(uint32_t(layout.valueOffset))});
    auto *global = new llvm::GlobalVariable(*lowering_.module_, mapDescTy_, true,
                                            llvm::GlobalValue::PrivateLinkage, init,
                                            "tz.layout." + type->name());
    global->setUnnamedAddr(llvm::GlobalValue::UnnamedAddr::Global);

    return global;
  };
  layout.descriptor = make(lowering_.isCyclic(type));
  layout.plainDescriptor = lowering_.isCyclic(type) ? make(false) : layout.descriptor;

  return layout;
}

llvm::Value *DictLowerer::dictField(llvm::Value *map, unsigned field) {
  if (!mapTy_) {
    llvm::Type *ptr = lowering_.builder_.getPtrTy(), *i32 = lowering_.builder_.getInt32Ty();
    mapTy_ = llvm::StructType::create(
        lowering_.ctx_,
        {lowering_.builder_.getInt64Ty(), ptr, ptr, ptr, i32, i32, i32, i32, i32, i32, ptr},
        "tz.map");
  }

  return lowering_.builder_.CreateStructGEP(mapTy_, map, field);
}

llvm::Value *DictLowerer::emitDictFind(llvm::Value *map, llvm::Value *key, const Type *keyType) {
  llvm::Type *ptr = lowering_.builder_.getPtrTy();
  if (keyType->isString()) {
    return lowering_.builder_.CreateCall(
        dictFunction("tz_map_find_str", ptr, {ptr, ptr, lowering_.builder_.getInt64Ty()}),
        {map, lowering_.stringLowerer().stringObj(key), lowering_.stringLowerer().stringMeta(key)},
        "entry");
  }

  return lowering_.builder_.CreateCall(dictFunction("tz_map_find", ptr, {ptr, ptr}),
                                       {map, lowering_.functionFrame().spill(key, "key")}, "entry");
}

llvm::Value *DictLowerer::emitDictAdd(llvm::Value *map, llvm::Value *key, const Type *keyType,
                                      llvm::Value **added) {
  llvm::Type *ptr = lowering_.builder_.getPtrTy();
  llvm::Value *flag =
      lowering_.functionFrame().createStackSlot(lowering_.builder_.getInt32Ty(), "added");
  llvm::Value *entry;
  if (keyType->isString()) {
    entry = lowering_.builder_.CreateCall(
        dictFunction("tz_map_add_str", ptr, {ptr, ptr, lowering_.builder_.getInt64Ty(), ptr}),
        {map, lowering_.stringLowerer().stringObj(key), lowering_.stringLowerer().stringMeta(key),
         flag},
        "entry");
  } else {
    entry = lowering_.builder_.CreateCall(dictFunction("tz_map_add", ptr, {ptr, ptr, ptr}),
                                          {map, lowering_.functionFrame().spill(key, "key"), flag},
                                          "entry");
  }
  if (added) {
    *added = lowering_.builder_.CreateICmpNE(
        lowering_.builder_.CreateLoad(lowering_.builder_.getInt32Ty(), flag),
        lowering_.builder_.getInt32(0), "added");
  }

  return entry;
}

llvm::Value *DictLowerer::entryKey(llvm::Value *entry, const Type *type) {
  return lowering_.builder_.CreateConstInBoundsGEP1_64(lowering_.builder_.getInt8Ty(), entry,
                                                       dictLayout(type).keyOffset, "key.addr");
}

llvm::Value *DictLowerer::entryValue(llvm::Value *entry, const Type *type) {
  return lowering_.builder_.CreateConstInBoundsGEP1_64(lowering_.builder_.getInt8Ty(), entry,
                                                       dictLayout(type).valueOffset, "value.addr");
}

void DictLowerer::emitEntryStored(llvm::Value *map, llvm::Value *entry) {
  llvm::Value *flags =
      lowering_.builder_.CreateLoad(lowering_.builder_.getInt32Ty(), dictField(map, kMapFlags));
  llvm::BasicBlock *envBlock = lowering_.functionFrame().newBlock("env.set");
  llvm::BasicBlock *doneBlock = lowering_.functionFrame().newBlock("env.done");
  lowering_.builder_.CreateCondBr(
      lowering_.builder_.CreateICmpNE(
          lowering_.builder_.CreateAnd(flags, lowering_.builder_.getInt32(kMapEnv)),
          lowering_.builder_.getInt32(0)),
      envBlock, doneBlock, llvm::MDBuilder(lowering_.ctx_).createUnlikelyBranchWeights());
  lowering_.functionFrame().switchTo(envBlock);
  llvm::Type *ptr = lowering_.builder_.getPtrTy();
  lowering_.builder_.CreateCall(
      dictFunction("tz_map_stored", lowering_.builder_.getVoidTy(), {ptr, ptr}), {map, entry});
  lowering_.builder_.CreateBr(doneBlock);
  lowering_.functionFrame().switchTo(doneBlock);
}

void DictLowerer::beginDictIteration(llvm::Value *slot) {
  llvm::Value *map = lowering_.builder_.CreateLoad(lowering_.builder_.getPtrTy(), slot, "map");
  llvm::Value *count = dictField(map, kMapIterating);
  lowering_.builder_.CreateStore(
      lowering_.builder_.CreateAdd(
          lowering_.builder_.CreateLoad(lowering_.builder_.getInt32Ty(), count),
          lowering_.builder_.getInt32(1)),
      count);
  lowering_.scopes_.back().dicts.push_back(slot);
}

void DictLowerer::emitDictLoop(llvm::Value *slot, const Type *type, llvm::Value *position,
                               llvm::function_ref<void(llvm::Value *)> each) {
  const Lowering::DictLayout &layout = dictLayout(type);
  llvm::Type *i32 = lowering_.builder_.getInt32Ty(), *i64 = lowering_.builder_.getInt64Ty();
  llvm::Value *indexSlot =
      position ? position : lowering_.functionFrame().createStackSlot(i32, "position");
  lowering_.builder_.CreateStore(lowering_.builder_.getInt32(0), indexSlot);
  llvm::BasicBlock *headBlock = lowering_.functionFrame().newBlock("dict.next");
  llvm::BasicBlock *entryBlock = lowering_.functionFrame().newBlock("dict.entry");
  llvm::BasicBlock *liveBlock = lowering_.functionFrame().newBlock("dict.live");
  llvm::BasicBlock *endBlock = lowering_.functionFrame().newBlock("dict.end");
  lowering_.functionFrame().emitBlock(headBlock);
  llvm::Value *map = lowering_.builder_.CreateLoad(lowering_.builder_.getPtrTy(), slot, "map");
  llvm::Value *index = lowering_.builder_.CreateLoad(i32, indexSlot, "position");
  llvm::Value *used = lowering_.builder_.CreateLoad(i32, dictField(map, kMapUsed), "used");
  lowering_.builder_.CreateCondBr(lowering_.builder_.CreateICmpULT(index, used), entryBlock,
                                  endBlock);
  lowering_.functionFrame().switchTo(entryBlock);
  lowering_.builder_.CreateStore(
      lowering_.builder_.CreateAdd(index, lowering_.builder_.getInt32(1)), indexSlot);
  llvm::Value *entries =
      lowering_.builder_.CreateLoad(lowering_.builder_.getPtrTy(), dictField(map, kMapEntries));
  llvm::Value *entry = lowering_.builder_.CreateInBoundsGEP(
      lowering_.builder_.getInt8Ty(), entries,
      lowering_.builder_.CreateMul(lowering_.builder_.CreateZExt(index, i64),
                                   lowering_.builder_.getInt64(layout.size)),
      "entry");
  llvm::Value *live =
      lowering_.builder_.CreateLoad(i32,
                                    lowering_.builder_.CreateConstInBoundsGEP1_64(
                                        lowering_.builder_.getInt8Ty(), entry, kEntryLive),
                                    "live");
  lowering_.builder_.CreateCondBr(
      lowering_.builder_.CreateICmpNE(live, lowering_.builder_.getInt32(0)), liveBlock, headBlock);
  lowering_.functionFrame().switchTo(liveBlock);
  each(entry);
  lowering_.functionFrame().branchTo(headBlock);
  lowering_.functionFrame().switchTo(endBlock);
}

Lowering::Producer DictLowerer::dictProducer(llvm::Value *slot, const Type *type, Builtin mode,
                                             llvm::Value *position) {
  Lowering::Producer producer;
  producer.run = [this, slot, type, mode, position](const Lowering::Consumer &consumer) {
    const Type *key = type->keyType(), *value = type->valueType();
    emitDictLoop(slot, type, position, [&](llvm::Value *entry) {
      auto load = [&](bool isKey) {
        const Type *t = isKey ? key : value;
        llvm::Value *v =
            lowering_.builder_.CreateLoad(lowering_.moduleBuilder().lower(t),
                                          isKey ? entryKey(entry, type) : entryValue(entry, type));
        lowering_.refCounts().retainValue(v, t);

        return v;
      };
      llvm::Value *given;
      if (mode == Builtin::MapKeys || mode == Builtin::SetValues) {
        given = load(true);
      } else if (mode == Builtin::MapValues) {
        given = load(false);
      } else {
        bool set = mode == Builtin::SetEntries;
        const Type *tuple = Type::tuple({key, set ? key : value});
        llvm::Value *k = load(true), *v = load(set);
        given = lowering_.objectLayouts().allocObject(tuple, {0, 1});
        lowering_.builder_.CreateStore(k,
                                       lowering_.objectLayouts().fieldAddress(given, tuple, "0"));
        lowering_.builder_.CreateStore(v,
                                       lowering_.objectLayouts().fieldAddress(given, tuple, "1"));
        lowering_.groupLowerer().adoptFields(given, tuple);
      }
      if (llvm::Value *sent = consumer.each(given)) {
        lowering_.refCounts().releaseValue(sent, consumer.sent);
      }
    });
  };

  return producer;
}

llvm::Value *DictLowerer::emitDictForEach(CallExpr *call) {
  Expr *receiver = cast<MemberExpr>(call->callee)->object;
  const Type *type = receiver->type;
  const Type *callbackType = call->args[0]->type;
  lowering_.functionFrame().pushScope();
  llvm::Value *slot = lowering_.functionFrame().createManagedSlot(type, "map");
  lowering_.builder_.CreateStore(lowering_.exprLowerer().emitExpr(receiver), slot);
  beginDictIteration(slot);
  llvm::Value *callback = lowering_.functionFrame().createManagedSlot(callbackType, "callback");
  lowering_.builder_.CreateStore(lowering_.exprLowerer().emitExpr(call->args[0]), callback);
  const Type *key = type->keyType();
  const Type *value = type->isSet() ? key : type->valueType();
  emitDictLoop(slot, type, nullptr, [&](llvm::Value *entry) {
    llvm::Value *v = lowering_.builder_.CreateLoad(lowering_.moduleBuilder().lower(value),
                                                   type->isSet() ? entryKey(entry, type)
                                                                 : entryValue(entry, type));
    llvm::Value *k =
        lowering_.builder_.CreateLoad(lowering_.moduleBuilder().lower(key), entryKey(entry, type));
    llvm::Value *map = lowering_.builder_.CreateLoad(lowering_.builder_.getPtrTy(), slot);
    lowering_.refCounts().retainValue(v, value);
    lowering_.refCounts().retainValue(k, key);
    lowering_.refCounts().retainValue(map, type);
    if (llvm::Value *result = lowering_.lazyChainLowerer().callCallback(
            callback, callbackType, {v, k, map}, {value, key, type})) {
      lowering_.refCounts().releaseValue(result, callbackType->ret());
    }
  });
  lowering_.functionFrame().popScope();

  return nullptr;
}

llvm::Value *DictLowerer::emitRecordLiteral(ObjectLitExpr *lit) {
  const Type *type = lit->type;
  const Lowering::DictLayout &layout = dictLayout(type);
  const Type *value = type->valueType(), *string = Type::string();
  llvm::Type *ptr = lowering_.builder_.getPtrTy();
  llvm::Value *map = lowering_.builder_.CreateCall(
      dictFunction("tz_map_new", ptr, {ptr}),
      {lowering_.makesMember(lit, type) ? layout.descriptor : layout.plainDescriptor}, "record");
  bool throws = llvm::any_of(lit->props, [&](const ObjectLitExpr::Property &p) {
    return lowering_.mayThrow().exprThrows(p.value) ||
           (p.key && lowering_.mayThrow().exprThrows(p.key));
  });
  Lowering::Held held = lowering_.throwLowerer().hold(map, type, throws);
  auto put = [&](llvm::Value *k, llvm::Value *v, const Type *from) {
    llvm::Value *entry = emitDictAdd(map, k, string);
    lowering_.storeLowerer().storeIn(map, type, entryValue(entry, type),
                                     lowering_.exprLowerer().emitConversion(v, from, value), value);
  };
  for (const ObjectLitExpr::Property &prop : lit->props) {
    if (!prop.spread) {
      Lowering::Operand k{lowering_.stringLowerer().stringLiteral(utf16(prop.name)), false, string};
      if (prop.key) {
        k = lowering_.refCounts().emitOperand(prop.key, AssignLowerer::cannotAssign(prop.value));
        lowering_.throwLowerer().holdOperand(k, lowering_.mayThrow().exprThrows(prop.value));
      }
      llvm::Value *v = lowering_.exprLowerer().emitExpr(prop.value);
      lowering_.throwLowerer().takeOperand(k);
      put(k.value, v, prop.value->type);
      lowering_.refCounts().dropOperand(k);
      continue;
    }
    const Type *source = prop.value->type;
    Lowering::Operand from = lowering_.refCounts().emitOperand(prop.value, true);
    auto omitted = [&](llvm::Value *k) -> llvm::Value * {
      llvm::Value *skip = lowering_.builder_.getFalse();
      for (llvm::StringRef name : prop.omit) {
        skip = lowering_.builder_.CreateOr(
            skip, lowering_.equalityLowerer().emitValuesEqual(
                      k, lowering_.stringLowerer().stringLiteral(utf16(name)), string));
      }

      return skip;
    };
    if (source->isRecord()) {
      llvm::Value *slot = lowering_.functionFrame().spill(from.value, "spread");
      emitDictLoop(slot, source, nullptr, [&](llvm::Value *entry) {
        const Type *sourceValue = source->valueType();
        llvm::Value *k = lowering_.builder_.CreateLoad(lowering_.moduleBuilder().lower(string),
                                                       entryKey(entry, source));
        llvm::BasicBlock *copyBlock = lowering_.functionFrame().newBlock("spread.copy");
        llvm::BasicBlock *nextBlock = lowering_.functionFrame().newBlock("spread.next");
        lowering_.builder_.CreateCondBr(omitted(k), nextBlock, copyBlock);
        lowering_.functionFrame().switchTo(copyBlock);
        llvm::Value *v = lowering_.builder_.CreateLoad(lowering_.moduleBuilder().lower(sourceValue),
                                                       entryValue(entry, source));
        lowering_.refCounts().retainValue(v, sourceValue);
        put(k, v, sourceValue);
        lowering_.builder_.CreateBr(nextBlock);
        lowering_.functionFrame().switchTo(nextBlock);
      });
    } else {
      for (const Field &f : source->fields()) {
        if (llvm::StringRef(f.name).starts_with("#") || llvm::is_contained(prop.omit, f.name)) {
          continue;
        }
        llvm::Value *v = lowering_.builder_.CreateLoad(
            lowering_.moduleBuilder().lower(f.type),
            lowering_.objectLayouts().fieldAddress(from.value, source, f.name));
        llvm::Value *k = lowering_.stringLowerer().stringLiteral(utf16(f.name));
        if (!f.type->isOptional() || value->isOptional()) {
          lowering_.refCounts().retainValue(v, f.type);
          put(k, v, f.type);
          continue;
        }
        llvm::BasicBlock *copyBlock = lowering_.functionFrame().newBlock("spread.copy");
        llvm::BasicBlock *nextBlock = lowering_.functionFrame().newBlock("spread.next");
        lowering_.builder_.CreateCondBr(lowering_.optionalLowerer().optionalHas(v, f.type),
                                        copyBlock, nextBlock);
        lowering_.functionFrame().switchTo(copyBlock);
        llvm::Value *present = lowering_.optionalLowerer().optionalValue(v, f.type);
        lowering_.refCounts().retainValue(present, f.type->inner());
        put(k, present, f.type->inner());
        lowering_.builder_.CreateBr(nextBlock);
        lowering_.functionFrame().switchTo(nextBlock);
      }
    }
    lowering_.refCounts().dropOperand(from);
  }

  return lowering_.throwLowerer().take(held);
}

}
