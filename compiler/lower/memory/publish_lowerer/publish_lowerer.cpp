#include "compiler/lower/memory/publish_lowerer/publish_lowerer.h"
#include "compiler/library/process/process_lowerer/process_lowerer.h"
#include "compiler/lower/functions/closure_lowerer/closure_lowerer.h"
#include "compiler/lower/functions/env_lowerer/env_lowerer.h"
#include "compiler/lower/layouts/object_layouts/object_layouts.h"
#include "compiler/lower/layouts/type_descriptors/type_descriptors.h"
#include "compiler/lower/memory/group_lowerer/group_lowerer.h"
#include "compiler/lower/memory/ref_counts/ref_counts.h"
#include "compiler/memory/memory_plan/memory_plan.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/IR/MDBuilder.h"
#include "llvm/Support/ConvertUTF.h"
#include <functional>
#include <map>
#include <set>

using llvm::BasicBlock;
using llvm::cast;
using llvm::dyn_cast;
using llvm::isa;
using llvm::Value;

namespace tz {

namespace {

llvm::Value *spillHere(llvm::IRBuilder<> &b, llvm::Value *value, const char *name) {
  llvm::BasicBlock &entry = b.GetInsertBlock()->getParent()->getEntryBlock();
  llvm::IRBuilder<> at(&entry, entry.begin());
  llvm::AllocaInst *slot = at.CreateAlloca(value->getType(), nullptr, name);
  b.CreateStore(value, slot);

  return slot;
}

enum class RefShape { None, Pointer, Function, Optional, Other };

}

PublishLowerer::PublishLowerer(Lowering &lowering) : lowering_(lowering) {}

llvm::Function *
PublishLowerer::publishFunction(const Type *type, llvm::StructType *layout,
                                llvm::ArrayRef<std::pair<const VarSymbol *, unsigned>> captures,
                                llvm::ArrayRef<std::pair<unsigned, const Type *>> values,
                                llvm::StringRef name) {
  llvm::Type *ptr = lowering_.builder_.getPtrTy();
  auto *fnType = llvm::FunctionType::get(lowering_.builder_.getVoidTy(), {ptr}, false);
  auto *fn = llvm::Function::Create(fnType, llvm::GlobalValue::InternalLinkage,
                                    "tz.publish." + name, lowering_.module_.get());
  fn->addFnAttr(llvm::Attribute::NoUnwind);
  llvm::IRBuilder<> b(llvm::BasicBlock::Create(lowering_.ctx_, "entry", fn));
  llvm::Value *object = fn->getArg(0);
  llvm::FunctionCallee value = lowering_.processLowerer().processFunction(
      "tz_publish_value", lowering_.builder_.getVoidTy(), {ptr, ptr});
  llvm::FunctionCallee ref = lowering_.processLowerer().processFunction(
      "tz_publish_object", lowering_.builder_.getVoidTy(), {ptr});
  auto one = [&](const Type *t, unsigned element) {
    if (!t->isManaged()) {
      return;
    }
    b.CreateCall(value, {lowering_.typeDescriptors().typeDescriptor(t),
                         b.CreateStructGEP(layout, object, element)});
  };
  if (type) {
    const Lowering::ObjectLayout &l = lowering_.objectLayouts().objectLayout(type);
    for (unsigned i = 0; i < type->fields().size(); ++i) {
      one(type->fields()[i].type, l.elements[i]);
    }
  }
  for (auto [var, element] : captures) {
    if (var->boxed()) {
      b.CreateCall(ref, {b.CreateLoad(ptr, b.CreateStructGEP(layout, object, element))});
    } else {
      one(ClosureLowerer::typeOf(var), element);
    }
  }
  for (auto [element, t] : values) {
    one(t, element);
  }
  b.CreateRetVoid();

  return fn;
}

void PublishLowerer::finishPublishTable(llvm::Instruction *init) {
  if (!lowering_.parallel_) {
    return;
  }
  llvm::Type *ptr = lowering_.builder_.getPtrTy();
  std::vector<llvm::Constant *> entries;
  auto *entryType = llvm::StructType::get(lowering_.ctx_, {ptr, ptr});
  llvm::DenseSet<llvm::Constant *> seen;
  auto add = [&](llvm::Constant *descriptor, llvm::Function *publish) {
    if (seen.insert(descriptor).second) {
      entries.push_back(llvm::ConstantStruct::get(entryType, {descriptor, publish}));
    }
  };
  std::map<const Type *, llvm::Function *> objects;
  auto objectPublish = [&](const Type *type) {
    llvm::Function *&publish = objects[type];
    if (!publish) {
      const Lowering::ObjectLayout &l = lowering_.objectLayouts().objectLayout(type);
      std::vector<std::pair<const VarSymbol *, unsigned>> captures;
      if (const ClassDecl *cls = type->classDecl(); cls && cls->group) {
        for (size_t i = 0; i < cls->group->captures.size(); ++i) {
          captures.push_back({cls->group->captures[i], unsigned(l.captures + i)});
        }
      }
      publish = publishFunction(type, l.type, captures, {}, "object");
    }

    return publish;
  };
  std::vector<std::pair<const Type *, llvm::Constant *>> layouts;
  for (const auto &[key, descriptor] : lowering_.layoutDescriptors_) {
    layouts.push_back({key.first, descriptor});
  }
  for (const auto &[type, descriptor] : layouts) {
    if (type->isObject()) {
      add(descriptor, objectPublish(type));
    }
  }
  for (const auto &[cls, descriptor] : lowering_.classDescriptors_) {
    add(descriptor, objectPublish(cls->type));
  }
  for (const auto &[key, layout] : lowering_.envLayouts_) {
    std::vector<std::pair<const VarSymbol *, unsigned>> captures;
    for (size_t i = 0; i < layout.fields.size(); ++i) {
      if (!lowering_.memory().uncounted.count({key, layout.fields[i]})) {
        captures.push_back({layout.fields[i], unsigned(i + 2)});
      }
    }
    add(layout.descriptor, publishFunction(nullptr, layout.type, captures, {}, "env"));
  }
  for (const auto &[type, drop] : lowering_.boxDrops_) {
    if (!type->isManaged()) {
      continue;
    }
    llvm::Function *publish =
        publishFunction(nullptr, lowering_.envLowerer().boxType(type), {}, {{2u, type}}, "box");
    if (llvm::Constant *descriptor = lowering_.dropDescriptors_.lookup(drop)) {
      add(descriptor, publish);
    }
    if (llvm::Constant *descriptor = lowering_.memberDescriptors_.lookup(drop)) {
      add(descriptor, publish);
    }
  }
  for (const Lowering::PublishedObject &o : lowering_.publishedObjects_) {
    add(o.descriptor, publishFunction(nullptr, o.type, {}, o.values, "other"));
  }
  auto *arrayType = llvm::ArrayType::get(entryType, entries.size());
  auto *table = new llvm::GlobalVariable(
      *lowering_.module_, arrayType, true, llvm::GlobalValue::PrivateLinkage,
      llvm::ConstantArray::get(arrayType, entries), "tz.publish.table");
  llvm::IRBuilder<> b(init->getNextNode());
  b.CreateCall(lowering_.processLowerer().processFunction("tz_publish_init",
                                                          lowering_.builder_.getVoidTy(),
                                                          {ptr, lowering_.builder_.getInt32Ty()}),
               {table, b.getInt32(uint32_t(entries.size()))});
}

void PublishLowerer::pinValue(llvm::Value *value, const Type *type) {
  if (!lowering_.memory().mayHoldMember(type) || isa<llvm::Constant>(value)) {
    return;
  }
  llvm::Type *ptr = lowering_.builder_.getPtrTy();
  lowering_.builder_.CreateCall(
      lowering_.groupLowerer().groupFunction("tz_group_pin_value", lowering_.builder_.getVoidTy(),
                                             {ptr, ptr}),
      {lowering_.typeDescriptors().typeDescriptor(type),
       spillHere(lowering_.builder_, value, "pinned")});
}

void PublishLowerer::endValue(llvm::Value *value, const Type *type) {
  if (!lowering_.memory().mayHoldMember(type)) {
    lowering_.refCounts().releaseValue(value, type);
    return;
  }
  llvm::Type *ptr = lowering_.builder_.getPtrTy();
  lowering_.builder_.CreateCall(
      lowering_.groupLowerer().groupFunction("tz_group_end_value", lowering_.builder_.getVoidTy(),
                                             {ptr, ptr}),
      {lowering_.typeDescriptors().typeDescriptor(type),
       spillHere(lowering_.builder_, value, "ended")});
}

}
