#include "compiler/lower/objects/dict_method_lowerer/dict_method_lowerer.h"
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

using llvm::cast;
using llvm::dyn_cast;

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

DictMethodLowerer::DictMethodLowerer(Lowering &owner)
    : lowering(owner) {}

llvm::Value *DictMethodLowerer::emitDictCall(CallExpr *call) {
  llvm::Type *ptr = this->lowering.builder.getPtrTy();
  llvm::Type *i32 = this->lowering.builder.getInt32Ty();
  switch (call->builtin()) {
    case Builtin::DictNew:
      return this->emitDictNew(call);
    case Builtin::DictGroupBy:
      return this->emitGroupBy(call);
    case Builtin::MapForEach:
      return this->lowering.dictLowerer().emitDictForEach(call);
    case Builtin::EnvMap:
      return this->lowering.builder.CreateCall(
          this->lowering.dictLowerer().dictFunction("tz_process_env_map", ptr, {ptr}),
          {this->lowering.dictLowerer().dictLayout(call->type).descriptor}, "env");
    default:
      break;
  }
  if (BuiltinGroups::isDictIterator(call->builtin())) {
    return this->lowering.iteratorLowerer().emitIteratorBuiltin(call);
  }
  Expr *receiver = cast<MemberExpr>(call->callee)->object;
  const Type *type = receiver->type;
  const Type *key = type->keyType();
  const Type *value = type->valueType();
  switch (call->builtin()) {
    case Builtin::MapSize: {
      Lowering::Operand map = this->lowering.refCounts().emitOperand(receiver, true);
      llvm::Value *size = this->lowering.builder.CreateLoad(
          i32, this->lowering.dictLowerer().dictField(map.value, kMapCount), "size");
      this->lowering.refCounts().dropOperand(map);
      return this->lowering.builder.CreateZExt(size, this->lowering.builder.getInt64Ty());
    }
    case Builtin::MapClear: {
      Lowering::Operand map = this->lowering.refCounts().emitOperand(receiver, true);
      this->lowering.builder.CreateCall(
          this->lowering.dictLowerer().dictFunction("tz_map_clear",
                                                    this->lowering.builder.getVoidTy(), {ptr}),
          {map.value});
      this->lowering.refCounts().dropOperand(map);
      return nullptr;
    }
    case Builtin::MapGet:
    case Builtin::MapHas:
    case Builtin::MapDelete: {
      Expr *keyExpr = call->args[0];
      Lowering::Operand map =
          this->lowering.refCounts().emitOperand(receiver, AssignLowerer::cannotAssign(keyExpr));
      Lowering::Operand k = this->lowering.refCounts().emitOperand(keyExpr, true);
      llvm::Value *result;
      if (call->builtin() == Builtin::MapDelete) {
        llvm::Value *removed;
        if (key->isString()) {
          removed = this->lowering.builder.CreateCall(
              this->lowering.dictLowerer().dictFunction(
                  "tz_map_delete_str", i32, {ptr, ptr, this->lowering.builder.getInt64Ty()}),
              {map.value, this->lowering.stringLowerer().stringObj(k.value),
               this->lowering.stringLowerer().stringMeta(k.value)});
        } else {
          removed = this->lowering.builder.CreateCall(
              this->lowering.dictLowerer().dictFunction("tz_map_delete", i32, {ptr, ptr}),
              {map.value, this->lowering.functionFrame().spill(k.value, "key")});
        }
        result = this->lowering.builder.CreateICmpNE(removed, this->lowering.builder.getInt32(0));
      } else {
        llvm::Value *entry = this->lowering.dictLowerer().emitDictFind(map.value, k.value, key);
        llvm::Value *found = this->lowering.builder.CreateIsNotNull(entry, "found");
        if (call->builtin() == Builtin::MapHas) {
          result = found;
        } else {
          result = this->lowering.arrayLowerer().emitCheckedRead(found, call->type, [&] {
            llvm::Value *v = this->lowering.builder.CreateLoad(
                this->lowering.moduleBuilder().lower(value),
                this->lowering.dictLowerer().entryValue(entry, type));
            this->lowering.refCounts().retainValue(v, value);
            return v;
          });
        }
      }
      this->lowering.refCounts().dropOperand(k);
      this->lowering.refCounts().dropOperand(map);
      return result;
    }
    case Builtin::MapSet:
    case Builtin::SetAdd:
    case Builtin::RecordSet: {
      Expr *keyExpr = call->args[0];
      Expr *valueExpr = call->args.size() > 1 ? call->args[1] : nullptr;
      bool later = valueExpr && !AssignLowerer::cannotAssign(valueExpr);
      Lowering::Operand map = this->lowering.refCounts().emitOperand(
          receiver, AssignLowerer::cannotAssign(keyExpr) && !later);
      Lowering::Operand k = this->lowering.refCounts().emitOperand(keyExpr, !later);
      llvm::Value *v = valueExpr ? this->lowering.exprLowerer().emitExpr(valueExpr) : nullptr;
      llvm::Value *entry = this->lowering.dictLowerer().emitDictAdd(map.value, k.value, key);
      if (v) {
        this->lowering.storeLowerer().storeIn(
            map.value, type, this->lowering.dictLowerer().entryValue(entry, type), v, value);
      }
      this->lowering.refCounts().dropOperand(k);
      if (call->builtin() == Builtin::RecordSet) {
        this->lowering.dictLowerer().emitEntryStored(map.value, entry);
        llvm::Value *result =
            this->lowering.builder.CreateLoad(this->lowering.moduleBuilder().lower(value),
                                              this->lowering.dictLowerer().entryValue(entry, type));
        this->lowering.refCounts().retainValue(result, value);
        this->lowering.refCounts().dropOperand(map);
        return result;
      }
      if (!map.owned) {
        this->lowering.refCounts().retainValue(map.value, type);
      }
      return map.value;
    }
    case Builtin::RecordNullishSet: {
      Expr *keyExpr = call->args[0];
      Expr *valueExpr = call->args[1];
      const Type *resultType = call->type;
      llvm::Value *m = this->lowering.exprLowerer().emitExpr(receiver);
      llvm::Value *kv = this->lowering.exprLowerer().emitExpr(keyExpr);
      llvm::Value *entry = this->lowering.dictLowerer().emitDictFind(m, kv, key);
      llvm::BasicBlock *lookBlock = this->lowering.functionFrame().newBlock("nullish.look");
      llvm::BasicBlock *keepBlock = this->lowering.functionFrame().newBlock("nullish.keep");
      llvm::BasicBlock *assignBlock = this->lowering.functionFrame().newBlock("nullish.assign");
      llvm::BasicBlock *endBlock = this->lowering.functionFrame().newBlock("nullish.end");
      this->lowering.builder.CreateCondBr(this->lowering.builder.CreateIsNotNull(entry), lookBlock,
                                          assignBlock);
      this->lowering.functionFrame().switchTo(lookBlock);
      llvm::Value *old =
          this->lowering.builder.CreateLoad(this->lowering.moduleBuilder().lower(value),
                                            this->lowering.dictLowerer().entryValue(entry, type));
      this->lowering.builder.CreateCondBr(
          value->isOptional() ? this->lowering.optionalLowerer().optionalHas(old, value)
                              : this->lowering.builder.getTrue(),
          keepBlock, assignBlock);
      this->lowering.functionFrame().switchTo(keepBlock);
      this->lowering.refCounts().retainValue(old, value);
      llvm::Value *kept = this->lowering.exprLowerer().emitConversion(old, value, resultType);
      llvm::BasicBlock *keepEnd = this->lowering.builder.GetInsertBlock();
      this->lowering.builder.CreateBr(endBlock);
      this->lowering.functionFrame().switchTo(assignBlock);
      llvm::Value *v = this->lowering.exprLowerer().emitExpr(valueExpr);
      llvm::Value *added = this->lowering.dictLowerer().emitDictAdd(m, kv, key);
      this->lowering.storeLowerer().storeIn(
          m, type, this->lowering.dictLowerer().entryValue(added, type), v, value);
      this->lowering.dictLowerer().emitEntryStored(m, added);
      this->lowering.refCounts().retainValue(v, value);
      llvm::Value *assigned = this->lowering.exprLowerer().emitConversion(v, value, resultType);
      llvm::BasicBlock *assignEnd = this->lowering.builder.GetInsertBlock();
      this->lowering.builder.CreateBr(endBlock);
      this->lowering.functionFrame().switchTo(endBlock);
      llvm::PHINode *result =
          this->lowering.builder.CreatePHI(this->lowering.moduleBuilder().lower(resultType), 2);
      result->addIncoming(kept, keepEnd);
      result->addIncoming(assigned, assignEnd);
      this->lowering.refCounts().releaseValue(kv, key);
      this->lowering.refCounts().releaseValue(m, type);
      return result;
    }
    default: {
      Expr *other = call->args[0];
      Lowering::Operand a =
          this->lowering.refCounts().emitOperand(receiver, AssignLowerer::cannotAssign(other));
      Lowering::Operand b = this->lowering.refCounts().emitOperand(other, true);
      llvm::Value *result;
      switch (call->builtin()) {
        case Builtin::SetUnion:
        case Builtin::SetIntersection:
        case Builtin::SetDifference:
        case Builtin::SetSymmetricDifference: {
          int op = int(call->builtin()) - int(Builtin::SetUnion);
          result = this->lowering.builder.CreateCall(
              this->lowering.dictLowerer().dictFunction("tz_map_set_op", ptr, {ptr, ptr, i32}),
              {a.value, b.value, this->lowering.builder.getInt32(uint32_t(op))}, "set");
          break;
        }
        case Builtin::SetIsSubsetOf:
        case Builtin::SetIsSupersetOf:
        case Builtin::SetIsDisjointFrom: {
          int op = int(call->builtin()) - int(Builtin::SetIsSubsetOf);
          result = this->lowering.builder.CreateICmpNE(
              this->lowering.builder.CreateCall(
                  this->lowering.dictLowerer().dictFunction("tz_map_set_test", i32,
                                                            {ptr, ptr, i32}),
                  {a.value, b.value, this->lowering.builder.getInt32(uint32_t(op))}),
              this->lowering.builder.getInt32(0));
          break;
        }
        default:
          llvm_unreachable("not a method of a dictionary");
      }
      this->lowering.refCounts().dropOperand(b);
      this->lowering.refCounts().dropOperand(a);
      return result;
    }
  }
}

llvm::Value *DictMethodLowerer::emitDictNew(CallExpr *call) {
  const Type *type = call->type;
  const Lowering::DictLayout &layout = this->lowering.dictLowerer().dictLayout(type);
  llvm::Type *ptr = this->lowering.builder.getPtrTy();
  llvm::FunctionCallee create = this->lowering.dictLowerer().dictFunction("tz_map_new", ptr, {ptr});
  llvm::Constant *descriptor =
      this->lowering.makesMember(call, type) ? layout.descriptor : layout.plainDescriptor;
  if (call->args.empty()) {
    llvm::Value *map = this->lowering.builder.CreateCall(create, {descriptor}, "map");
    this->lowering.groupLowerer().noteUnstored(map, call);
    return map;
  }
  Expr *source = call->args[0];
  if (auto *iterator = dyn_cast<CallExpr>(source);
      iterator &&
      (iterator->builtin() == Builtin::MapEntries || iterator->builtin() == Builtin::SetValues)) {
    Expr *from = cast<MemberExpr>(iterator->callee)->object;
    if (from->type == type) {
      Lowering::Operand map = this->lowering.refCounts().emitOperand(from, true);
      llvm::Value *copy = this->lowering.builder.CreateCall(
          this->lowering.dictLowerer().dictFunction("tz_map_copy", ptr, {ptr, ptr}),
          {map.value, descriptor}, "map");
      this->lowering.refCounts().dropOperand(map);
      return copy;
    }
  }
  const Type *element = source->type->isArray()    ? source->type->inner()
                        : source->type->isString() ? source->type
                                                   : source->type->yielded();
  this->lowering.functionFrame().pushScope();
  Lowering::Producer producer = this->lowering.inlineGeneratorLowerer().prepare(source);
  llvm::Value *map = this->lowering.builder.CreateCall(create, {descriptor}, "map");
  this->lowering.groupLowerer().noteUnstored(map, call);
  size_t depth = this->lowering.scopes.size();
  llvm::BasicBlock *endBlock = this->lowering.functionFrame().newBlock("dict.filled");
  const Type *key = type->keyType();
  const Type *value = type->valueType();
  Lowering::Consumer consumer;
  consumer.stop = [this, depth, endBlock] {
    this->lowering.functionFrame().emitCleanups(depth);
    this->lowering.builder.CreateBr(endBlock);
  };
  consumer.each = [&](llvm::Value *given) -> llvm::Value * {
    if (type->isSet()) {
      llvm::Value *k = this->lowering.exprLowerer().emitConversion(given, element, key);
      this->lowering.dictLowerer().emitDictAdd(map, k, key);
      this->lowering.refCounts().releaseValue(k, key);
      return nullptr;
    }
    const Type *kType = element->fields()[0].type;
    const Type *vType = element->fields()[1].type;
    llvm::Value *k = this->lowering.builder.CreateLoad(
        this->lowering.moduleBuilder().lower(kType),
        this->lowering.objectLayouts().fieldAddress(given, element, "0"));
    llvm::Value *v = this->lowering.builder.CreateLoad(
        this->lowering.moduleBuilder().lower(vType),
        this->lowering.objectLayouts().fieldAddress(given, element, "1"));
    this->lowering.refCounts().retainValue(k, kType);
    this->lowering.refCounts().retainValue(v, vType);
    this->lowering.refCounts().releaseValue(given, element);
    k = this->lowering.exprLowerer().emitConversion(k, kType, key);
    v = this->lowering.exprLowerer().emitConversion(v, vType, value);
    llvm::Value *entry = this->lowering.dictLowerer().emitDictAdd(map, k, key);
    this->lowering.storeLowerer().storeIn(
        map, type, this->lowering.dictLowerer().entryValue(entry, type), v, value);
    this->lowering.refCounts().releaseValue(k, key);

    return nullptr;
  };
  producer.run(consumer);
  this->lowering.functionFrame().emitBlock(endBlock);
  llvm::Value *result = map;
  this->lowering.functionFrame().popScope();

  return result;
}

llvm::Value *DictMethodLowerer::emitGroupBy(CallExpr *call) {
  const Type *type = call->type;
  const Lowering::DictLayout &layout = this->lowering.dictLowerer().dictLayout(type);
  llvm::Type *ptr = this->lowering.builder.getPtrTy();
  Expr *source = call->args[0];
  const Type *element = source->type->isArray()    ? source->type->inner()
                        : source->type->isString() ? source->type
                                                   : source->type->yielded();
  const Type *callbackType = call->args[1]->type;
  const Type *key = type->keyType();
  const Type *group = type->valueType();
  this->lowering.functionFrame().pushScope();
  Lowering::Producer producer = this->lowering.inlineGeneratorLowerer().prepare(source);
  llvm::Value *callback =
      this->lowering.functionFrame().createManagedSlot(callbackType, "callback");
  this->lowering.builder.CreateStore(this->lowering.exprLowerer().emitExpr(call->args[1]),
                                     callback);
  llvm::Value *map = this->lowering.builder.CreateCall(
      this->lowering.dictLowerer().dictFunction("tz_map_new", ptr, {ptr}), {layout.descriptor},
      "map");
  if (type->isRecord()) {
    this->lowering.builder.CreateStore(this->lowering.builder.getInt32(4),
                                       this->lowering.dictLowerer().dictField(map, kMapFlags));
  }
  llvm::Value *counter = this->lowering.functionFrame().createStackSlot(
      this->lowering.builder.getInt64Ty(), "counter");
  this->lowering.builder.CreateStore(this->lowering.builder.getInt64(0), counter);
  size_t depth = this->lowering.scopes.size();
  llvm::BasicBlock *endBlock = this->lowering.functionFrame().newBlock("group.done");
  Lowering::Consumer consumer;
  consumer.stop = [this, depth, endBlock] {
    this->lowering.functionFrame().emitCleanups(depth);
    this->lowering.builder.CreateBr(endBlock);
  };
  consumer.each = [&](llvm::Value *item) -> llvm::Value * {
    llvm::Value *count =
        this->lowering.builder.CreateLoad(this->lowering.builder.getInt64Ty(), counter);
    this->lowering.builder.CreateStore(
        this->lowering.builder.CreateAdd(count, this->lowering.builder.getInt64(1)), counter);
    this->lowering.refCounts().retainValue(item, element);
    llvm::Value *k = this->lowering.lazyChainLowerer().callCallback(
        callback, callbackType, {item, count}, {element, Type::intTy()});
    llvm::Value *added;
    llvm::Value *entry = this->lowering.dictLowerer().emitDictAdd(map, k, key, &added);
    this->lowering.refCounts().releaseValue(k, key);
    llvm::Value *slot = this->lowering.dictLowerer().entryValue(entry, type);
    llvm::BasicBlock *newBlock_ = this->lowering.functionFrame().newBlock("group.new");
    llvm::BasicBlock *pushBlock = this->lowering.functionFrame().newBlock("group.push");
    this->lowering.builder.CreateCondBr(added, newBlock_, pushBlock);
    this->lowering.functionFrame().switchTo(newBlock_);
    llvm::Value *fresh = this->lowering.groupLowerer().newArray(
        group, this->lowering.builder.getInt64(0), this->lowering.builder.getInt64(0));
    this->lowering.builder.CreateStore(fresh, slot);
    if (this->lowering.isCyclic(type)) {
      this->lowering.storeLowerer().groupStore(map, fresh, group);
    }
    this->lowering.builder.CreateBr(pushBlock);
    this->lowering.functionFrame().switchTo(pushBlock);
    this->lowering.arrayLowerer().emitPush(this->lowering.builder.CreateLoad(ptr, slot, "group"),
                                           {item}, group->inner());

    return nullptr;
  };
  producer.run(consumer);
  this->lowering.functionFrame().emitBlock(endBlock);
  llvm::Value *result = map;
  this->lowering.functionFrame().popScope();

  return result;
}

llvm::Value *DictMethodLowerer::emitDictObjectCall(CallExpr *call) {
  Lowering::Operand map = this->lowering.refCounts().emitOperand(call->args[0], true);
  const Type *element = call->type->inner();
  llvm::Type *ptr = this->lowering.builder.getPtrTy();
  llvm::Value *result;
  if (call->builtin() == Builtin::ObjectKeys) {
    result = this->lowering.builder.CreateCall(
        this->lowering.dictLowerer().dictFunction("tz_map_keys", ptr, {ptr, ptr}),
        {map.value, this->lowering.typeDescriptors().typeDescriptor(element)}, "keys");
  } else if (call->builtin() == Builtin::ObjectValues) {
    result = this->lowering.builder.CreateCall(
        this->lowering.dictLowerer().dictFunction("tz_map_values", ptr, {ptr, ptr}),
        {map.value, this->lowering.typeDescriptors().typeDescriptor(element)}, "values");
  } else {
    uint64_t size = this->lowering.module_->getDataLayout().getTypeAllocSize(
        this->lowering.objectLayouts().objectLayout(element).type);
    result = this->lowering.builder.CreateCall(
        this->lowering.dictLowerer().dictFunction(
            "tz_map_entries", ptr, {ptr, ptr, ptr, this->lowering.builder.getInt64Ty()}),
        {map.value, this->lowering.typeDescriptors().typeDescriptor(element),
         this->lowering.objectLayouts().layoutDescriptor(element, {0, 1}),
         this->lowering.builder.getInt64(size)},
        "entries");
  }
  this->lowering.refCounts().dropOperand(map);

  return result;
}

}
