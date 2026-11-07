#include "compiler/lower/objects/dict_lowerer/dict_lowerer.h"
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

using llvm::cast;

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

}

std::u16string DictLowerer::utf16(llvm::StringRef text) {
  llvm::SmallVector<llvm::UTF16, 32> units;
  llvm::convertUTF8ToUTF16String(text, units);

  return std::u16string(units.begin(), units.end());
}

DictLowerer::DictLowerer(Lowering &owner)
    : lowering(owner) {}

llvm::FunctionCallee DictLowerer::dictFunction(llvm::StringRef name, llvm::Type *ret,
                                               llvm::ArrayRef<llvm::Type *> params) {
  llvm::FunctionCallee callee = this->lowering.module_->getOrInsertFunction(
      name, llvm::FunctionType::get(ret, params, false));
  cast<llvm::Function>(callee.getCallee())->addFnAttr(llvm::Attribute::NoUnwind);

  return callee;
}

const Lowering::DictLayout &DictLowerer::dictLayout(const Type *type) {
  auto [it, inserted] = this->dictLayouts.try_emplace(type);
  Lowering::DictLayout &layout = it->second;
  if (!inserted) {
    return layout;
  }
  llvm::Type *ptr = this->lowering.builder_.getPtrTy();
  llvm::Type *i32 = this->lowering.builder_.getInt32Ty();
  const Type *key = type->keyType();
  const Type *value = type->valueType();
  std::vector<llvm::Type *> elements{i32, i32, this->lowering.moduleBuilder().lower(key)};
  if (!type->isSet()) {
    elements.push_back(this->lowering.moduleBuilder().lower(value));
  }
  layout.entry = llvm::StructType::get(this->lowering.ctx_, elements);
  const llvm::DataLayout &data = this->lowering.module_->getDataLayout();
  const llvm::StructLayout *offsets = data.getStructLayout(layout.entry);
  layout.keyOffset = offsets->getElementOffset(2);
  layout.valueOffset = type->isSet() ? uint64_t(0) : offsets->getElementOffset(3);
  layout.size = data.getTypeAllocSize(layout.entry);

  auto *dropType = llvm::FunctionType::get(this->lowering.builder_.getVoidTy(), {ptr}, false);
  llvm::Constant *drop = cast<llvm::Constant>(
      this->lowering.module_->getOrInsertFunction("tz_map_drop", dropType).getCallee());
  llvm::Constant *null = llvm::ConstantPointerNull::get(ptr);
  uint32_t kind = type->isRecord() ? 0 : type->isMap() ? 1 : 2;
  if (!this->mapDescTy) {
    this->mapDescTy = llvm::StructType::create(
        this->lowering.ctx_, {this->lowering.layoutDescTy_, ptr, ptr, i32, i32, i32, i32},
        "tz.maplayout");
  }
  auto make = [&](bool member) {
    uint32_t flags = kLayoutDict | (member ? 0x80000000u : 0u);
    llvm::Constant *base = llvm::ConstantStruct::get(
        this->lowering.layoutDescTy_,
        {drop, null, this->lowering.builder_.getInt32(0), this->lowering.builder_.getInt32(flags),
         null, this->lowering.builder_.getInt32(0),
         this->lowering.builder_.getInt32(this->lowering.unionLowerer().plainTypeId(type))});
    llvm::Constant *init = llvm::ConstantStruct::get(
        this->mapDescTy,
        {base, this->lowering.typeDescriptors().typeDescriptor(key),
         type->isSet() ? null : this->lowering.typeDescriptors().typeDescriptor(value),
         this->lowering.builder_.getInt32(kind),
         this->lowering.builder_.getInt32(uint32_t(layout.size)),
         this->lowering.builder_.getInt32(uint32_t(layout.keyOffset)),
         this->lowering.builder_.getInt32(uint32_t(layout.valueOffset))});
    auto *global = new llvm::GlobalVariable(*this->lowering.module_, this->mapDescTy, true,
                                            llvm::GlobalValue::PrivateLinkage, init,
                                            "tz.layout." + type->name());
    global->setUnnamedAddr(llvm::GlobalValue::UnnamedAddr::Global);

    return global;
  };
  layout.descriptor = make(this->lowering.isCyclic(type));
  layout.plainDescriptor = this->lowering.isCyclic(type) ? make(false) : layout.descriptor;

  return layout;
}

llvm::Value *DictLowerer::dictField(llvm::Value *map, unsigned field) {
  if (!this->mapTy) {
    llvm::Type *ptr = this->lowering.builder_.getPtrTy();
    llvm::Type *i32 = this->lowering.builder_.getInt32Ty();
    this->mapTy = llvm::StructType::create(
        this->lowering.ctx_,
        {this->lowering.builder_.getInt64Ty(), ptr, ptr, ptr, i32, i32, i32, i32, i32, i32, ptr},
        "tz.map");
  }

  return this->lowering.builder_.CreateStructGEP(this->mapTy, map, field);
}

llvm::Value *DictLowerer::emitDictFind(llvm::Value *map, llvm::Value *key, const Type *keyType) {
  llvm::Type *ptr = this->lowering.builder_.getPtrTy();
  if (keyType->isString()) {
    return this->lowering.builder_.CreateCall(
        this->dictFunction("tz_map_find_str", ptr,
                           {ptr, ptr, this->lowering.builder_.getInt64Ty()}),
        {map, this->lowering.stringLowerer().stringObj(key),
         this->lowering.stringLowerer().stringMeta(key)},
        "entry");
  }

  return this->lowering.builder_.CreateCall(this->dictFunction("tz_map_find", ptr, {ptr, ptr}),
                                            {map, this->lowering.functionFrame().spill(key, "key")},
                                            "entry");
}

llvm::Value *DictLowerer::emitDictAdd(llvm::Value *map, llvm::Value *key, const Type *keyType,
                                      llvm::Value **added) {
  llvm::Type *ptr = this->lowering.builder_.getPtrTy();
  llvm::Value *flag =
      this->lowering.functionFrame().createStackSlot(this->lowering.builder_.getInt32Ty(), "added");
  llvm::Value *entry;
  if (keyType->isString()) {
    entry = this->lowering.builder_.CreateCall(
        this->dictFunction("tz_map_add_str", ptr,
                           {ptr, ptr, this->lowering.builder_.getInt64Ty(), ptr}),
        {map, this->lowering.stringLowerer().stringObj(key),
         this->lowering.stringLowerer().stringMeta(key), flag},
        "entry");
  } else {
    entry = this->lowering.builder_.CreateCall(
        this->dictFunction("tz_map_add", ptr, {ptr, ptr, ptr}),
        {map, this->lowering.functionFrame().spill(key, "key"), flag}, "entry");
  }
  if (added) {
    *added = this->lowering.builder_.CreateICmpNE(
        this->lowering.builder_.CreateLoad(this->lowering.builder_.getInt32Ty(), flag),
        this->lowering.builder_.getInt32(0), "added");
  }

  return entry;
}

llvm::Value *DictLowerer::entryKey(llvm::Value *entry, const Type *type) {
  return this->lowering.builder_.CreateConstInBoundsGEP1_64(
      this->lowering.builder_.getInt8Ty(), entry, this->dictLayout(type).keyOffset, "key.addr");
}

llvm::Value *DictLowerer::entryValue(llvm::Value *entry, const Type *type) {
  return this->lowering.builder_.CreateConstInBoundsGEP1_64(
      this->lowering.builder_.getInt8Ty(), entry, this->dictLayout(type).valueOffset, "value.addr");
}

void DictLowerer::emitEntryStored(llvm::Value *map, llvm::Value *entry) {
  llvm::Value *flags = this->lowering.builder_.CreateLoad(this->lowering.builder_.getInt32Ty(),
                                                          this->dictField(map, kMapFlags));
  llvm::BasicBlock *envBlock = this->lowering.functionFrame().newBlock("env.set");
  llvm::BasicBlock *doneBlock = this->lowering.functionFrame().newBlock("env.done");
  this->lowering.builder_.CreateCondBr(
      this->lowering.builder_.CreateICmpNE(
          this->lowering.builder_.CreateAnd(flags, this->lowering.builder_.getInt32(kMapEnv)),
          this->lowering.builder_.getInt32(0)),
      envBlock, doneBlock, llvm::MDBuilder(this->lowering.ctx_).createUnlikelyBranchWeights());
  this->lowering.functionFrame().switchTo(envBlock);
  llvm::Type *ptr = this->lowering.builder_.getPtrTy();
  this->lowering.builder_.CreateCall(
      this->dictFunction("tz_map_stored", this->lowering.builder_.getVoidTy(), {ptr, ptr}),
      {map, entry});
  this->lowering.builder_.CreateBr(doneBlock);
  this->lowering.functionFrame().switchTo(doneBlock);
}

void DictLowerer::beginDictIteration(llvm::Value *slot) {
  llvm::Value *map =
      this->lowering.builder_.CreateLoad(this->lowering.builder_.getPtrTy(), slot, "map");
  llvm::Value *count = this->dictField(map, kMapIterating);
  this->lowering.builder_.CreateStore(
      this->lowering.builder_.CreateAdd(
          this->lowering.builder_.CreateLoad(this->lowering.builder_.getInt32Ty(), count),
          this->lowering.builder_.getInt32(1)),
      count);
  this->lowering.scopes_.back().dicts.push_back(slot);
}

void DictLowerer::emitDictLoop(llvm::Value *slot, const Type *type, llvm::Value *position,
                               llvm::function_ref<void(llvm::Value *)> each) {
  const Lowering::DictLayout &layout = this->dictLayout(type);
  llvm::Type *i32 = this->lowering.builder_.getInt32Ty();
  llvm::Type *i64 = this->lowering.builder_.getInt64Ty();
  llvm::Value *indexSlot =
      position ? position : this->lowering.functionFrame().createStackSlot(i32, "position");
  this->lowering.builder_.CreateStore(this->lowering.builder_.getInt32(0), indexSlot);
  llvm::BasicBlock *headBlock = this->lowering.functionFrame().newBlock("dict.next");
  llvm::BasicBlock *entryBlock = this->lowering.functionFrame().newBlock("dict.entry");
  llvm::BasicBlock *liveBlock = this->lowering.functionFrame().newBlock("dict.live");
  llvm::BasicBlock *endBlock = this->lowering.functionFrame().newBlock("dict.end");
  this->lowering.functionFrame().emitBlock(headBlock);
  llvm::Value *map =
      this->lowering.builder_.CreateLoad(this->lowering.builder_.getPtrTy(), slot, "map");
  llvm::Value *index = this->lowering.builder_.CreateLoad(i32, indexSlot, "position");
  llvm::Value *used =
      this->lowering.builder_.CreateLoad(i32, this->dictField(map, kMapUsed), "used");
  this->lowering.builder_.CreateCondBr(this->lowering.builder_.CreateICmpULT(index, used),
                                       entryBlock, endBlock);
  this->lowering.functionFrame().switchTo(entryBlock);
  this->lowering.builder_.CreateStore(
      this->lowering.builder_.CreateAdd(index, this->lowering.builder_.getInt32(1)), indexSlot);
  llvm::Value *entries = this->lowering.builder_.CreateLoad(this->lowering.builder_.getPtrTy(),
                                                            this->dictField(map, kMapEntries));
  llvm::Value *entry = this->lowering.builder_.CreateInBoundsGEP(
      this->lowering.builder_.getInt8Ty(), entries,
      this->lowering.builder_.CreateMul(this->lowering.builder_.CreateZExt(index, i64),
                                        this->lowering.builder_.getInt64(layout.size)),
      "entry");
  llvm::Value *live = this->lowering.builder_.CreateLoad(
      i32,
      this->lowering.builder_.CreateConstInBoundsGEP1_64(this->lowering.builder_.getInt8Ty(), entry,
                                                         kEntryLive),
      "live");
  this->lowering.builder_.CreateCondBr(
      this->lowering.builder_.CreateICmpNE(live, this->lowering.builder_.getInt32(0)), liveBlock,
      headBlock);
  this->lowering.functionFrame().switchTo(liveBlock);
  each(entry);
  this->lowering.functionFrame().branchTo(headBlock);
  this->lowering.functionFrame().switchTo(endBlock);
}

Lowering::Producer DictLowerer::dictProducer(llvm::Value *slot, const Type *type, Builtin mode,
                                             llvm::Value *position) {
  Lowering::Producer producer;
  producer.run = [this, slot, type, mode, position](const Lowering::Consumer &consumer) {
    const Type *key = type->keyType();
    const Type *value = type->valueType();
    this->emitDictLoop(slot, type, position, [&](llvm::Value *entry) {
      auto load = [&](bool isKey) {
        const Type *t = isKey ? key : value;
        llvm::Value *v = this->lowering.builder_.CreateLoad(this->lowering.moduleBuilder().lower(t),
                                                            isKey ? this->entryKey(entry, type)
                                                                  : this->entryValue(entry, type));
        this->lowering.refCounts().retainValue(v, t);

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
        llvm::Value *k = load(true);
        llvm::Value *v = load(set);
        given = this->lowering.objectLayouts().allocObject(tuple, {0, 1});
        this->lowering.builder_.CreateStore(
            k, this->lowering.objectLayouts().fieldAddress(given, tuple, "0"));
        this->lowering.builder_.CreateStore(
            v, this->lowering.objectLayouts().fieldAddress(given, tuple, "1"));
        this->lowering.groupLowerer().adoptFields(given, tuple);
      }
      llvm::Value *sent = consumer.each(given);
      if (sent) {
        this->lowering.refCounts().releaseValue(sent, consumer.sent);
      }
    });
  };

  return producer;
}

llvm::Value *DictLowerer::emitDictForEach(CallExpr *call) {
  Expr *receiver = cast<MemberExpr>(call->callee)->object;
  const Type *type = receiver->type;
  const Type *callbackType = call->args[0]->type;
  this->lowering.functionFrame().pushScope();
  llvm::Value *slot = this->lowering.functionFrame().createManagedSlot(type, "map");
  this->lowering.builder_.CreateStore(this->lowering.exprLowerer().emitExpr(receiver), slot);
  this->beginDictIteration(slot);
  llvm::Value *callback =
      this->lowering.functionFrame().createManagedSlot(callbackType, "callback");
  this->lowering.builder_.CreateStore(this->lowering.exprLowerer().emitExpr(call->args[0]),
                                      callback);
  const Type *key = type->keyType();
  const Type *value = type->isSet() ? key : type->valueType();
  this->emitDictLoop(slot, type, nullptr, [&](llvm::Value *entry) {
    llvm::Value *v = this->lowering.builder_.CreateLoad(
        this->lowering.moduleBuilder().lower(value),
        type->isSet() ? this->entryKey(entry, type) : this->entryValue(entry, type));
    llvm::Value *k = this->lowering.builder_.CreateLoad(this->lowering.moduleBuilder().lower(key),
                                                        this->entryKey(entry, type));
    llvm::Value *map = this->lowering.builder_.CreateLoad(this->lowering.builder_.getPtrTy(), slot);
    this->lowering.refCounts().retainValue(v, value);
    this->lowering.refCounts().retainValue(k, key);
    this->lowering.refCounts().retainValue(map, type);
    if (llvm::Value *result = this->lowering.lazyChainLowerer().callCallback(
            callback, callbackType, {v, k, map}, {value, key, type})) {
      this->lowering.refCounts().releaseValue(result, callbackType->ret());
    }
  });
  this->lowering.functionFrame().popScope();

  return nullptr;
}

llvm::Value *DictLowerer::emitRecordFromObject(llvm::Value *object, const Type *from,
                                               const Type *to) {
  const Lowering::DictLayout &layout = this->dictLayout(to);
  const Type *value = to->valueType();
  const Type *string = Type::string();
  llvm::Type *ptr = this->lowering.builder_.getPtrTy();
  llvm::Value *map = this->lowering.builder_.CreateCall(
      this->dictFunction("tz_map_new", ptr, {ptr}), {layout.plainDescriptor}, "record");
  for (const Field &f : from->fields()) {
    llvm::Value *v = this->lowering.builder_.CreateLoad(
        this->lowering.moduleBuilder().lower(f.type),
        this->lowering.objectLayouts().fieldAddress(object, from, f.name));
    llvm::Value *k = this->lowering.stringLowerer().stringLiteral(DictLowerer::utf16(f.name));
    auto put = [&](llvm::Value *item, const Type *type) {
      llvm::Value *entry = this->emitDictAdd(map, k, string);
      this->lowering.storeLowerer().storeIn(
          map, to, this->entryValue(entry, to),
          this->lowering.exprLowerer().emitConversion(item, type, value), value);
    };
    if (!f.type->isOptional() || value->isOptional()) {
      this->lowering.refCounts().retainValue(v, f.type);
      put(v, f.type);
      continue;
    }
    llvm::BasicBlock *copyBlock = this->lowering.functionFrame().newBlock("record.copy");
    llvm::BasicBlock *nextBlock = this->lowering.functionFrame().newBlock("record.next");
    this->lowering.builder_.CreateCondBr(this->lowering.optionalLowerer().optionalHas(v, f.type),
                                         copyBlock, nextBlock);
    this->lowering.functionFrame().switchTo(copyBlock);
    llvm::Value *present = this->lowering.optionalLowerer().optionalValue(v, f.type);
    this->lowering.refCounts().retainValue(present, f.type->inner());
    put(present, f.type->inner());
    this->lowering.builder_.CreateBr(nextBlock);
    this->lowering.functionFrame().switchTo(nextBlock);
  }
  this->lowering.refCounts().releaseValue(object, from);

  return map;
}

llvm::Value *DictLowerer::emitRecordLiteral(ObjectLitExpr *lit) {
  const Type *type = lit->type;
  const Lowering::DictLayout &layout = this->dictLayout(type);
  const Type *value = type->valueType();
  const Type *string = Type::string();
  llvm::Type *ptr = this->lowering.builder_.getPtrTy();
  llvm::Value *map = this->lowering.builder_.CreateCall(
      this->dictFunction("tz_map_new", ptr, {ptr}),
      {this->lowering.makesMember(lit, type) ? layout.descriptor : layout.plainDescriptor},
      "record");
  auto put = [&](llvm::Value *k, llvm::Value *v, const Type *from) {
    llvm::Value *entry = this->emitDictAdd(map, k, string);
    this->lowering.storeLowerer().storeIn(
        map, type, this->entryValue(entry, type),
        this->lowering.exprLowerer().emitConversion(v, from, value), value);
  };
  for (const ObjectLitExpr::Property &prop : lit->props) {
    if (!prop.spread()) {
      Lowering::Operand k{
          this->lowering.stringLowerer().stringLiteral(DictLowerer::utf16(prop.name)), false,
          string};
      if (prop.key()) {
        k = this->lowering.refCounts().emitOperand(prop.key(),
                                                   AssignLowerer::cannotAssign(prop.value));
      }
      llvm::Value *v = this->lowering.exprLowerer().emitExpr(prop.value);
      put(k.value, v, prop.value->type);
      this->lowering.refCounts().dropOperand(k);
      continue;
    }
    const Type *source = prop.value->type;
    Lowering::Operand from = this->lowering.refCounts().emitOperand(prop.value, true);
    auto omitted = [&](llvm::Value *k) -> llvm::Value * {
      llvm::Value *skip = this->lowering.builder_.getFalse();
      for (llvm::StringRef name : prop.omit()) {
        skip = this->lowering.builder_.CreateOr(
            skip,
            this->lowering.equalityLowerer().emitValuesEqual(
                k, this->lowering.stringLowerer().stringLiteral(DictLowerer::utf16(name)), string));
      }

      return skip;
    };
    if (source->isRecord()) {
      llvm::Value *slot = this->lowering.functionFrame().spill(from.value, "spread");
      this->emitDictLoop(slot, source, nullptr, [&](llvm::Value *entry) {
        const Type *sourceValue = source->valueType();
        llvm::Value *k = this->lowering.builder_.CreateLoad(
            this->lowering.moduleBuilder().lower(string), this->entryKey(entry, source));
        llvm::BasicBlock *copyBlock = this->lowering.functionFrame().newBlock("spread.copy");
        llvm::BasicBlock *nextBlock = this->lowering.functionFrame().newBlock("spread.next");
        this->lowering.builder_.CreateCondBr(omitted(k), nextBlock, copyBlock);
        this->lowering.functionFrame().switchTo(copyBlock);
        llvm::Value *v = this->lowering.builder_.CreateLoad(
            this->lowering.moduleBuilder().lower(sourceValue), this->entryValue(entry, source));
        this->lowering.refCounts().retainValue(v, sourceValue);
        put(k, v, sourceValue);
        this->lowering.builder_.CreateBr(nextBlock);
        this->lowering.functionFrame().switchTo(nextBlock);
      });
    } else {
      for (const Field &f : source->fields()) {
        if (llvm::is_contained(prop.omit(), f.name)) {
          continue;
        }
        llvm::Value *v = this->lowering.builder_.CreateLoad(
            this->lowering.moduleBuilder().lower(f.type),
            this->lowering.objectLayouts().fieldAddress(from.value, source, f.name));
        llvm::Value *k = this->lowering.stringLowerer().stringLiteral(DictLowerer::utf16(f.name));
        if (!f.type->isOptional() || value->isOptional()) {
          this->lowering.refCounts().retainValue(v, f.type);
          put(k, v, f.type);
          continue;
        }
        llvm::BasicBlock *copyBlock = this->lowering.functionFrame().newBlock("spread.copy");
        llvm::BasicBlock *nextBlock = this->lowering.functionFrame().newBlock("spread.next");
        this->lowering.builder_.CreateCondBr(
            this->lowering.optionalLowerer().optionalHas(v, f.type), copyBlock, nextBlock);
        this->lowering.functionFrame().switchTo(copyBlock);
        llvm::Value *present = this->lowering.optionalLowerer().optionalValue(v, f.type);
        this->lowering.refCounts().retainValue(present, f.type->inner());
        put(k, present, f.type->inner());
        this->lowering.builder_.CreateBr(nextBlock);
        this->lowering.functionFrame().switchTo(nextBlock);
      }
    }
    this->lowering.refCounts().dropOperand(from);
  }

  return map;
}

}
