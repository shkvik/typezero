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
#include <map>

using llvm::isa;

namespace tz {

namespace {

enum class RefShape { None, Pointer, Function, Optional, Other };

}

llvm::Value *PublishLowerer::spillHere(llvm::IRBuilder<> &b, llvm::Value *value, const char *name) {
  llvm::BasicBlock &entry = b.GetInsertBlock()->getParent()->getEntryBlock();
  llvm::IRBuilder<> at(&entry, entry.begin());
  llvm::AllocaInst *slot = at.CreateAlloca(value->getType(), nullptr, name);
  b.CreateStore(value, slot);

  return slot;
}

PublishLowerer::PublishLowerer(Lowering &owner)
    : lowering(owner) {}

llvm::Function *
PublishLowerer::publishFunction(const Type *type, llvm::StructType *layout,
                                llvm::ArrayRef<std::pair<const VarSymbol *, unsigned>> captures,
                                llvm::ArrayRef<std::pair<unsigned, const Type *>> values,
                                llvm::StringRef name) {
  llvm::Type *ptr = this->lowering.builder_.getPtrTy();
  auto *fnType = llvm::FunctionType::get(this->lowering.builder_.getVoidTy(), {ptr}, false);
  auto *fn = llvm::Function::Create(fnType, llvm::GlobalValue::InternalLinkage,
                                    "tz.publish." + name, this->lowering.module_.get());
  fn->addFnAttr(llvm::Attribute::NoUnwind);
  llvm::IRBuilder<> b(llvm::BasicBlock::Create(this->lowering.ctx_, "entry", fn));
  llvm::Value *object = fn->getArg(0);
  llvm::FunctionCallee value = this->lowering.processLowerer().processFunction(
      "tz_publish_value", this->lowering.builder_.getVoidTy(), {ptr, ptr});
  llvm::FunctionCallee ref = this->lowering.processLowerer().processFunction(
      "tz_publish_object", this->lowering.builder_.getVoidTy(), {ptr});
  auto one = [&](const Type *t, unsigned element) {
    if (!t->isManaged()) {
      return;
    }
    b.CreateCall(value, {this->lowering.typeDescriptors().typeDescriptor(t),
                         b.CreateStructGEP(layout, object, element)});
  };
  if (type) {
    const Lowering::ObjectLayout &l = this->lowering.objectLayouts().objectLayout(type);
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
  if (!this->lowering.parallel_) {
    return;
  }
  llvm::Type *ptr = this->lowering.builder_.getPtrTy();
  std::vector<llvm::Constant *> entries;
  auto *entryType = llvm::StructType::get(this->lowering.ctx_, {ptr, ptr});
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
      const Lowering::ObjectLayout &l = this->lowering.objectLayouts().objectLayout(type);
      std::vector<std::pair<const VarSymbol *, unsigned>> captures;
      const ClassDecl *cls = type->classDecl();
      if (cls && cls->group) {
        for (size_t i = 0; i < cls->group->captures.size(); ++i) {
          captures.push_back({cls->group->captures[i], unsigned(l.captures + i)});
        }
      }
      publish = this->publishFunction(type, l.type, captures, {}, "object");
    }

    return publish;
  };
  std::vector<std::pair<const Type *, llvm::Constant *>> layouts;
  for (const auto &[key, descriptor] : this->lowering.layoutDescriptors_) {
    layouts.push_back({key.first, descriptor});
  }
  for (const auto &[type, descriptor] : layouts) {
    if (type->isObject()) {
      add(descriptor, objectPublish(type));
    }
  }
  for (const auto &[cls, descriptor] : this->lowering.classDescriptors_) {
    add(descriptor, objectPublish(cls->type));
  }
  for (const auto &[key, layout] : this->lowering.envLayouts_) {
    std::vector<std::pair<const VarSymbol *, unsigned>> captures;
    for (size_t i = 0; i < layout.fields.size(); ++i) {
      if (!this->lowering.memory().uncounted.count({key, layout.fields[i]})) {
        captures.push_back({layout.fields[i], unsigned(i + 2)});
      }
    }
    add(layout.descriptor, this->publishFunction(nullptr, layout.type, captures, {}, "env"));
  }
  for (const auto &[type, drop] : this->lowering.boxDrops_) {
    if (!type->isManaged()) {
      continue;
    }
    llvm::Function *publish = this->publishFunction(
        nullptr, this->lowering.envLowerer().boxType(type), {}, {{2u, type}}, "box");
    llvm::Constant *dropDescriptor = this->lowering.dropDescriptors_.lookup(drop);
    if (dropDescriptor) {
      add(dropDescriptor, publish);
    }
    llvm::Constant *memberDescriptor = this->lowering.memberDescriptors_.lookup(drop);
    if (memberDescriptor) {
      add(memberDescriptor, publish);
    }
  }
  for (const Lowering::PublishedObject &o : this->lowering.publishedObjects_) {
    add(o.descriptor, this->publishFunction(nullptr, o.type, {}, o.values, "other"));
  }
  auto *arrayType = llvm::ArrayType::get(entryType, entries.size());
  auto *table = new llvm::GlobalVariable(
      *this->lowering.module_, arrayType, true, llvm::GlobalValue::PrivateLinkage,
      llvm::ConstantArray::get(arrayType, entries), "tz.publish.table");
  llvm::IRBuilder<> b(init->getNextNode());
  b.CreateCall(this->lowering.processLowerer().processFunction(
                   "tz_publish_init", this->lowering.builder_.getVoidTy(),
                   {ptr, this->lowering.builder_.getInt32Ty()}),
               {table, b.getInt32(uint32_t(entries.size()))});
}

void PublishLowerer::pinValue(llvm::Value *value, const Type *type) {
  if (!this->lowering.memory().mayCarryMember(type) || isa<llvm::Constant>(value)) {
    return;
  }
  llvm::Type *ptr = this->lowering.builder_.getPtrTy();
  this->lowering.builder_.CreateCall(
      this->lowering.groupLowerer().groupFunction("tz_group_pin_value",
                                                  this->lowering.builder_.getVoidTy(), {ptr, ptr}),
      {this->lowering.typeDescriptors().typeDescriptor(type),
       PublishLowerer::spillHere(this->lowering.builder_, value, "pinned")});
}

void PublishLowerer::endValue(llvm::Value *value, const Type *type) {
  if (!this->lowering.memory().mayCarryMember(type)) {
    this->lowering.refCounts().releaseValue(value, type);
    return;
  }
  llvm::Type *ptr = this->lowering.builder_.getPtrTy();
  this->lowering.builder_.CreateCall(
      this->lowering.groupLowerer().groupFunction("tz_group_end_value",
                                                  this->lowering.builder_.getVoidTy(), {ptr, ptr}),
      {this->lowering.typeDescriptors().typeDescriptor(type),
       PublishLowerer::spillHere(this->lowering.builder_, value, "ended")});
}

}
