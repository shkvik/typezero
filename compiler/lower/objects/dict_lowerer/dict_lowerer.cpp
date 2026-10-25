#include "compiler/lower/objects/dict_lowerer/dict_lowerer.h"
#include "compiler/lower/module_builder/module_builder.h"
#include "compiler/lower/function_frame/function_frame.h"
#include "compiler/lower/memory/ref_counts/ref_counts.h"
#include "compiler/lower/types/optional_lowerer/optional_lowerer.h"
#include "compiler/lower/expr/expr_lowerer/expr_lowerer.h"
#include "compiler/lower/expr/equality_lowerer/equality_lowerer.h"
#include "compiler/lower/expr/assign_lowerer/assign_lowerer.h"
#include "compiler/lower/values/string_lowerer/string_lowerer.h"
#include "compiler/lower/layouts/type_descriptors/type_descriptors.h"
#include "compiler/lower/memory/group_lowerer/group_lowerer.h"
#include "compiler/lower/memory/store_lowerer/store_lowerer.h"
#include "compiler/lower/iterators/lazy_chain_lowerer/lazy_chain_lowerer.h"
#include "compiler/lower/layouts/object_layouts/object_layouts.h"
#include "compiler/lower/errors/may_throw/may_throw.h"
#include "compiler/lower/errors/throw_lowerer/throw_lowerer.h"
#include "compiler/lower/types/union_lowerer/union_lowerer.h"
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

DictLowerer::DictLowerer(CodeGen &codegen) : codegen_(codegen) {}

llvm::FunctionCallee DictLowerer::dictFunction(llvm::StringRef name, llvm::Type *ret,
                                           llvm::ArrayRef<llvm::Type *> params) {
  llvm::FunctionCallee callee =
      codegen_.module_->getOrInsertFunction(name, llvm::FunctionType::get(ret, params, false));
  cast<llvm::Function>(callee.getCallee())->addFnAttr(llvm::Attribute::NoUnwind);

  return callee;
}

const CodeGen::DictLayout &DictLowerer::dictLayout(const Type *type) {
  auto [it, inserted] = dictLayouts_.try_emplace(type);
  CodeGen::DictLayout &layout = it->second;
  if (!inserted) {
    return layout;
  }
  llvm::Type *ptr = codegen_.builder_.getPtrTy(), *i32 = codegen_.builder_.getInt32Ty();
  const Type *key = type->keyType(), *value = type->valueType();
  std::vector<llvm::Type *> elements{i32, i32, codegen_.moduleBuilder().lower(key)};
  if (!type->isSet()) {
    elements.push_back(codegen_.moduleBuilder().lower(value));
  }
  layout.entry = llvm::StructType::get(codegen_.ctx_, elements);
  const llvm::DataLayout &data = codegen_.module_->getDataLayout();
  const llvm::StructLayout *offsets = data.getStructLayout(layout.entry);
  layout.keyOffset = offsets->getElementOffset(2);
  layout.valueOffset = type->isSet() ? uint64_t(0) : offsets->getElementOffset(3);
  layout.size = data.getTypeAllocSize(layout.entry);

  auto *dropType = llvm::FunctionType::get(codegen_.builder_.getVoidTy(), {ptr}, false);
  llvm::Constant *drop =
      cast<llvm::Constant>(codegen_.module_->getOrInsertFunction("tz_map_drop", dropType).getCallee());
  llvm::Constant *null = llvm::ConstantPointerNull::get(ptr);
  // Its number tells it from the other objects of a union (see plainTypeId).
  uint32_t kind = type->isRecord() ? 0 : type->isMap() ? 1 : 2; // TZ_MAP_RECORD...
  if (!mapDescTy_) {
    mapDescTy_ = llvm::StructType::create(codegen_.ctx_, {codegen_.layoutDescTy_, ptr, ptr, i32, i32, i32, i32},
                                          "tz.maplayout");
  }
  // TZ_LAYOUT_GROUP: the maps are members of groups
  // (runtime/core/group/group.c).
  auto make = [&](bool member) {
    uint32_t flags = kLayoutDict | (member ? 0x80000000u : 0u);
    llvm::Constant *base = llvm::ConstantStruct::get(
        codegen_.layoutDescTy_, {drop, null, codegen_.builder_.getInt32(0), codegen_.builder_.getInt32(flags), null,
                        codegen_.builder_.getInt32(0), codegen_.builder_.getInt32(codegen_.unionLowerer().plainTypeId(type))});
    llvm::Constant *init = llvm::ConstantStruct::get(
        mapDescTy_, {base, codegen_.typeDescriptors().typeDescriptor(key), type->isSet() ? null : codegen_.typeDescriptors().typeDescriptor(value),
                     codegen_.builder_.getInt32(kind), codegen_.builder_.getInt32(uint32_t(layout.size)),
                     codegen_.builder_.getInt32(uint32_t(layout.keyOffset)),
                     codegen_.builder_.getInt32(uint32_t(layout.valueOffset))});
    auto *global = new llvm::GlobalVariable(*codegen_.module_, mapDescTy_, /*isConstant=*/true,
                                            llvm::GlobalValue::PrivateLinkage, init,
                                            "tz.layout." + type->name());
    global->setUnnamedAddr(llvm::GlobalValue::UnnamedAddr::Global);

    return global;
  };
  layout.descriptor = make(codegen_.isCyclic(type));
  layout.plainDescriptor = codegen_.isCyclic(type) ? make(false) : layout.descriptor;

  return layout;
}

llvm::Value *DictLowerer::dictField(llvm::Value *map, unsigned field) {
  if (!mapTy_) {
    llvm::Type *ptr = codegen_.builder_.getPtrTy(), *i32 = codegen_.builder_.getInt32Ty();
    mapTy_ = llvm::StructType::create(
        codegen_.ctx_, {codegen_.builder_.getInt64Ty(), ptr, ptr, ptr, i32, i32, i32, i32, i32, i32, ptr}, "tz.map");
  }

  return codegen_.builder_.CreateStructGEP(mapTy_, map, field);
}

llvm::Value *DictLowerer::emitDictFind(llvm::Value *map, llvm::Value *key, const Type *keyType) {
  llvm::Type *ptr = codegen_.builder_.getPtrTy();
  if (keyType->isString()) {
    return codegen_.builder_.CreateCall(
        dictFunction("tz_map_find_str", ptr, {ptr, ptr, codegen_.builder_.getInt64Ty()}),
        {map, codegen_.stringLowerer().stringObj(key), codegen_.stringLowerer().stringMeta(key)}, "entry");
  }

  return codegen_.builder_.CreateCall(dictFunction("tz_map_find", ptr, {ptr, ptr}), {map, codegen_.functionFrame().spill(key, "key")},
                             "entry");
}

llvm::Value *DictLowerer::emitDictAdd(llvm::Value *map, llvm::Value *key, const Type *keyType,
                                  llvm::Value **added) {
  llvm::Type *ptr = codegen_.builder_.getPtrTy();
  llvm::Value *flag = codegen_.functionFrame().createStackSlot(codegen_.builder_.getInt32Ty(), "added");
  llvm::Value *entry;
  if (keyType->isString()) {
    entry = codegen_.builder_.CreateCall(
        dictFunction("tz_map_add_str", ptr, {ptr, ptr, codegen_.builder_.getInt64Ty(), ptr}),
        {map, codegen_.stringLowerer().stringObj(key), codegen_.stringLowerer().stringMeta(key), flag}, "entry");
  } else {
    entry = codegen_.builder_.CreateCall(dictFunction("tz_map_add", ptr, {ptr, ptr, ptr}),
                                {map, codegen_.functionFrame().spill(key, "key"), flag}, "entry");
  }
  if (added) {
    *added = codegen_.builder_.CreateICmpNE(codegen_.builder_.CreateLoad(codegen_.builder_.getInt32Ty(), flag),
                                   codegen_.builder_.getInt32(0), "added");
  }

  return entry;
}

llvm::Value *DictLowerer::entryKey(llvm::Value *entry, const Type *type) {
  return codegen_.builder_.CreateConstInBoundsGEP1_64(codegen_.builder_.getInt8Ty(), entry,
                                             dictLayout(type).keyOffset, "key.addr");
}

llvm::Value *DictLowerer::entryValue(llvm::Value *entry, const Type *type) {
  return codegen_.builder_.CreateConstInBoundsGEP1_64(codegen_.builder_.getInt8Ty(), entry,
                                             dictLayout(type).valueOffset, "value.addr");
}

void DictLowerer::emitEntryStored(llvm::Value *map, llvm::Value *entry) {
  llvm::Value *flags = codegen_.builder_.CreateLoad(codegen_.builder_.getInt32Ty(), dictField(map, kMapFlags));
  llvm::BasicBlock *envBlock = codegen_.functionFrame().newBlock("env.set");
  llvm::BasicBlock *doneBlock = codegen_.functionFrame().newBlock("env.done");
  codegen_.builder_.CreateCondBr(codegen_.builder_.CreateICmpNE(codegen_.builder_.CreateAnd(flags, codegen_.builder_.getInt32(kMapEnv)),
                                              codegen_.builder_.getInt32(0)),
                        envBlock, doneBlock, llvm::MDBuilder(codegen_.ctx_).createUnlikelyBranchWeights());
  codegen_.functionFrame().switchTo(envBlock);
  llvm::Type *ptr = codegen_.builder_.getPtrTy();
  codegen_.builder_.CreateCall(dictFunction("tz_map_stored", codegen_.builder_.getVoidTy(), {ptr, ptr}),
                      {map, entry});
  codegen_.builder_.CreateBr(doneBlock);
  codegen_.functionFrame().switchTo(doneBlock);
}

void DictLowerer::beginDictIteration(llvm::Value *slot) {
  llvm::Value *map = codegen_.builder_.CreateLoad(codegen_.builder_.getPtrTy(), slot, "map");
  llvm::Value *count = dictField(map, kMapIterating);
  codegen_.builder_.CreateStore(
      codegen_.builder_.CreateAdd(codegen_.builder_.CreateLoad(codegen_.builder_.getInt32Ty(), count), codegen_.builder_.getInt32(1)),
      count);
  codegen_.scopes_.back().dicts.push_back(slot);
}

void DictLowerer::emitDictLoop(llvm::Value *slot, const Type *type, llvm::Value *position,
                           llvm::function_ref<void(llvm::Value *)> each) {
  const CodeGen::DictLayout &layout = dictLayout(type);
  llvm::Type *i32 = codegen_.builder_.getInt32Ty(), *i64 = codegen_.builder_.getInt64Ty();
  llvm::Value *indexSlot = position ? position : codegen_.functionFrame().createStackSlot(i32, "position");
  codegen_.builder_.CreateStore(codegen_.builder_.getInt32(0), indexSlot);
  llvm::BasicBlock *headBlock = codegen_.functionFrame().newBlock("dict.next");
  llvm::BasicBlock *entryBlock = codegen_.functionFrame().newBlock("dict.entry");
  llvm::BasicBlock *liveBlock = codegen_.functionFrame().newBlock("dict.live");
  llvm::BasicBlock *endBlock = codegen_.functionFrame().newBlock("dict.end");
  codegen_.functionFrame().emitBlock(headBlock);
  // Where the entries are, and how many, may change in the body.
  llvm::Value *map = codegen_.builder_.CreateLoad(codegen_.builder_.getPtrTy(), slot, "map");
  llvm::Value *index = codegen_.builder_.CreateLoad(i32, indexSlot, "position");
  llvm::Value *used = codegen_.builder_.CreateLoad(i32, dictField(map, kMapUsed), "used");
  codegen_.builder_.CreateCondBr(codegen_.builder_.CreateICmpULT(index, used), entryBlock, endBlock);
  codegen_.functionFrame().switchTo(entryBlock);
  codegen_.builder_.CreateStore(codegen_.builder_.CreateAdd(index, codegen_.builder_.getInt32(1)), indexSlot);
  llvm::Value *entries = codegen_.builder_.CreateLoad(codegen_.builder_.getPtrTy(), dictField(map, kMapEntries));
  llvm::Value *entry = codegen_.builder_.CreateInBoundsGEP(
      codegen_.builder_.getInt8Ty(), entries,
      codegen_.builder_.CreateMul(codegen_.builder_.CreateZExt(index, i64), codegen_.builder_.getInt64(layout.size)), "entry");
  llvm::Value *live = codegen_.builder_.CreateLoad(
      i32, codegen_.builder_.CreateConstInBoundsGEP1_64(codegen_.builder_.getInt8Ty(), entry, kEntryLive), "live");
  codegen_.builder_.CreateCondBr(codegen_.builder_.CreateICmpNE(live, codegen_.builder_.getInt32(0)), liveBlock, headBlock);
  codegen_.functionFrame().switchTo(liveBlock);
  each(entry);
  codegen_.functionFrame().branchTo(headBlock);
  codegen_.functionFrame().switchTo(endBlock);
}

CodeGen::Producer DictLowerer::dictProducer(llvm::Value *slot, const Type *type, Builtin mode,
                                        llvm::Value *position) {
  CodeGen::Producer producer;
  producer.run = [this, slot, type, mode, position](const CodeGen::Consumer &consumer) {
    const Type *key = type->keyType(), *value = type->valueType();
    emitDictLoop(slot, type, position, [&](llvm::Value *entry) {
      auto load = [&](bool isKey) {
        const Type *t = isKey ? key : value;
        llvm::Value *v =
            codegen_.builder_.CreateLoad(codegen_.moduleBuilder().lower(t), isKey ? entryKey(entry, type) : entryValue(entry, type));
        codegen_.refCounts().retainValue(v, t);

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
        given = codegen_.objectLayouts().allocObject(tuple, {0, 1});
        codegen_.builder_.CreateStore(k, codegen_.objectLayouts().fieldAddress(given, tuple, "0"));
        codegen_.builder_.CreateStore(v, codegen_.objectLayouts().fieldAddress(given, tuple, "1"));
        codegen_.groupLowerer().adoptFields(given, tuple);
      }
      if (llvm::Value *sent = consumer.each(given)) {
        codegen_.refCounts().releaseValue(sent, consumer.sent);
      }
    });
  };

  return producer;
}

// `m.forEach(f)`: f(value, key, map) for each entry, or f(value, value,
// set), as an iteration, which sees the entries the callback adds.
llvm::Value *DictLowerer::emitDictForEach(CallExpr *call) {
  Expr *receiver = cast<MemberExpr>(call->callee)->object;
  const Type *type = receiver->type;
  const Type *callbackType = call->args[0]->type;
  codegen_.functionFrame().pushScope();
  llvm::Value *slot = codegen_.functionFrame().createManagedSlot(type, "map");
  codegen_.builder_.CreateStore(codegen_.exprLowerer().emitExpr(receiver), slot);
  beginDictIteration(slot);
  llvm::Value *callback = codegen_.functionFrame().createManagedSlot(callbackType, "callback");
  codegen_.builder_.CreateStore(codegen_.exprLowerer().emitExpr(call->args[0]), callback);
  const Type *key = type->keyType();
  const Type *value = type->isSet() ? key : type->valueType();
  emitDictLoop(slot, type, nullptr, [&](llvm::Value *entry) {
    llvm::Value *v = codegen_.builder_.CreateLoad(codegen_.moduleBuilder().lower(value), type->isSet() ? entryKey(entry, type)
                                                                     : entryValue(entry, type));
    llvm::Value *k = codegen_.builder_.CreateLoad(codegen_.moduleBuilder().lower(key), entryKey(entry, type));
    llvm::Value *map = codegen_.builder_.CreateLoad(codegen_.builder_.getPtrTy(), slot);
    codegen_.refCounts().retainValue(v, value);
    codegen_.refCounts().retainValue(k, key);
    codegen_.refCounts().retainValue(map, type);
    if (llvm::Value *result =
            codegen_.lazyChainLowerer().callCallback(callback, callbackType, {v, k, map}, {value, key, type})) {
      codegen_.refCounts().releaseValue(result, callbackType->ret());
    }
  });
  codegen_.functionFrame().popScope();

  return nullptr;
}

// `{ a: 1, [k]: v, ...r, ...o }` where a Record is expected: the keys in
// order, each later one replacing the value of an earlier one but not its
// place, as in JavaScript.
llvm::Value *DictLowerer::emitRecordLiteral(ObjectLitExpr *lit) {
  const Type *type = lit->type;
  const CodeGen::DictLayout &layout = dictLayout(type);
  const Type *value = type->valueType(), *string = Type::string();
  llvm::Type *ptr = codegen_.builder_.getPtrTy();
  llvm::Value *map = codegen_.builder_.CreateCall(
      dictFunction("tz_map_new", ptr, {ptr}),
      {codegen_.makesMember(lit, type) ? layout.descriptor : layout.plainDescriptor}, "record");
  bool throws = llvm::any_of(lit->props, [&](const ObjectLitExpr::Property &p) {
    return codegen_.mayThrow().exprThrows(p.value) || (p.key && codegen_.mayThrow().exprThrows(p.key));
  });
  CodeGen::Held held = codegen_.throwLowerer().hold(map, type, throws);
  auto put = [&](llvm::Value *k, llvm::Value *v, const Type *from) {
    llvm::Value *entry = emitDictAdd(map, k, string);
    codegen_.storeLowerer().storeIn(map, type, entryValue(entry, type), codegen_.exprLowerer().emitConversion(v, from, value), value);
  };
  for (const ObjectLitExpr::Property &prop : lit->props) {
    if (!prop.spread) {
      CodeGen::Operand k{codegen_.stringLowerer().stringLiteral(utf16(prop.name)), false, string};
      if (prop.key) {
        k = codegen_.refCounts().emitOperand(prop.key, /*canBorrow=*/AssignLowerer::cannotAssign(prop.value));
        codegen_.throwLowerer().holdOperand(k, codegen_.mayThrow().exprThrows(prop.value));
      }
      llvm::Value *v = codegen_.exprLowerer().emitExpr(prop.value);
      codegen_.throwLowerer().takeOperand(k);
      put(k.value, v, prop.value->type);
      codegen_.refCounts().dropOperand(k);
      continue;
    }
    const Type *source = prop.value->type;
    CodeGen::Operand from = codegen_.refCounts().emitOperand(prop.value, /*canBorrow=*/true);
    auto omitted = [&](llvm::Value *k) -> llvm::Value * {
      llvm::Value *skip = codegen_.builder_.getFalse();
      for (llvm::StringRef name : prop.omit) {
        skip = codegen_.builder_.CreateOr(skip, codegen_.equalityLowerer().emitValuesEqual(k, codegen_.stringLowerer().stringLiteral(utf16(name)), string));
      }

      return skip;
    };
    if (source->isRecord()) {
      // Nothing runs while the entries are copied, so the source needs no
      // count of its iteration.
      llvm::Value *slot = codegen_.functionFrame().spill(from.value, "spread");
      emitDictLoop(slot, source, nullptr, [&](llvm::Value *entry) {
        const Type *sourceValue = source->valueType();
        llvm::Value *k = codegen_.builder_.CreateLoad(codegen_.moduleBuilder().lower(string), entryKey(entry, source));
        llvm::BasicBlock *copyBlock = codegen_.functionFrame().newBlock("spread.copy");
        llvm::BasicBlock *nextBlock = codegen_.functionFrame().newBlock("spread.next");
        codegen_.builder_.CreateCondBr(omitted(k), nextBlock, copyBlock);
        codegen_.functionFrame().switchTo(copyBlock);
        llvm::Value *v = codegen_.builder_.CreateLoad(codegen_.moduleBuilder().lower(sourceValue), entryValue(entry, source));
        codegen_.refCounts().retainValue(v, sourceValue);
        put(k, v, sourceValue);
        codegen_.builder_.CreateBr(nextBlock);
        codegen_.functionFrame().switchTo(nextBlock);
      });
    } else {
      // The fields that hold a value, in the order of the type.
      for (const Field &f : source->fields()) {
        if (llvm::StringRef(f.name).starts_with("#") || llvm::is_contained(prop.omit, f.name)) {
          continue;
        }
        llvm::Value *v =
            codegen_.builder_.CreateLoad(codegen_.moduleBuilder().lower(f.type), codegen_.objectLayouts().fieldAddress(from.value, source, f.name));
        llvm::Value *k = codegen_.stringLowerer().stringLiteral(utf16(f.name));
        if (!f.type->isOptional() || value->isOptional()) {
          codegen_.refCounts().retainValue(v, f.type);
          put(k, v, f.type);
          continue;
        }
        llvm::BasicBlock *copyBlock = codegen_.functionFrame().newBlock("spread.copy");
        llvm::BasicBlock *nextBlock = codegen_.functionFrame().newBlock("spread.next");
        codegen_.builder_.CreateCondBr(codegen_.optionalLowerer().optionalHas(v, f.type), copyBlock, nextBlock);
        codegen_.functionFrame().switchTo(copyBlock);
        llvm::Value *present = codegen_.optionalLowerer().optionalValue(v, f.type);
        codegen_.refCounts().retainValue(present, f.type->inner());
        put(k, present, f.type->inner());
        codegen_.builder_.CreateBr(nextBlock);
        codegen_.functionFrame().switchTo(nextBlock);
      }
    }
    codegen_.refCounts().dropOperand(from);
  }

  return codegen_.throwLowerer().take(held);
}

} // namespace tz
