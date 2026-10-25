#include "compiler/lower/memory/publish_lowerer/publish_lowerer.h"
#include "compiler/lower/memory/ref_counts/ref_counts.h"
#include "compiler/lower/layouts/type_descriptors/type_descriptors.h"
#include "compiler/lower/functions/closure_lowerer/closure_lowerer.h"
#include "compiler/lower/functions/env_lowerer/env_lowerer.h"
#include "compiler/lower/memory/memory_analysis/memory_analysis.h"
#include "compiler/lower/memory/group_lowerer/group_lowerer.h"
#include "compiler/lower/layouts/object_layouts/object_layouts.h"
#include "compiler/library/process/process_lowerer/process_lowerer.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/IR/MDBuilder.h"
#include "llvm/Support/ConvertUTF.h"
#include <functional>
#include <map>
#include <set>

using llvm::cast;
using llvm::dyn_cast;
using llvm::isa;
using llvm::BasicBlock;
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

// How CodeGen walks the references in a value for the group functions: the
// reference itself, or the environment of a function; anything else goes to
// the runtime by its descriptor.
enum class RefShape { None, Pointer, Function, Optional, Other };

} // namespace

PublishLowerer::PublishLowerer(CodeGen &codegen) : codegen_(codegen) {}

// void publish(ptr object): every value the object holds, to the runtime,
// which makes it shared (runtime/core/publish/publish.c); the boxes of the
// variables it captures as objects.
llvm::Function *
PublishLowerer::publishFunction(const Type *type, llvm::StructType *layout,
                         llvm::ArrayRef<std::pair<const VarSymbol *, unsigned>> captures,
                         llvm::ArrayRef<std::pair<unsigned, const Type *>> values,
                         llvm::StringRef name) {
  llvm::Type *ptr = codegen_.builder_.getPtrTy();
  auto *fnType = llvm::FunctionType::get(codegen_.builder_.getVoidTy(), {ptr}, false);
  auto *fn = llvm::Function::Create(fnType, llvm::GlobalValue::InternalLinkage,
                                    "tz.publish." + name, codegen_.module_.get());
  fn->addFnAttr(llvm::Attribute::NoUnwind);
  llvm::IRBuilder<> b(llvm::BasicBlock::Create(codegen_.ctx_, "entry", fn));
  llvm::Value *object = fn->getArg(0);
  llvm::FunctionCallee value =
      codegen_.processLowerer().processFunction("tz_publish_value", codegen_.builder_.getVoidTy(), {ptr, ptr});
  llvm::FunctionCallee ref = codegen_.processLowerer().processFunction("tz_publish_object", codegen_.builder_.getVoidTy(), {ptr});
  auto one = [&](const Type *t, unsigned element) {
    if (!t->isManaged()) {
      return;
    }
    b.CreateCall(value, {codegen_.typeDescriptors().typeDescriptor(t), b.CreateStructGEP(layout, object, element)});
  };
  if (type) {
    const CodeGen::ObjectLayout &l = codegen_.objectLayouts().objectLayout(type);
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
  if (!codegen_.parallel_) {
    return;
  }
  llvm::Type *ptr = codegen_.builder_.getPtrTy();
  std::vector<llvm::Constant *> entries;
  auto *entryType = llvm::StructType::get(codegen_.ctx_, {ptr, ptr});
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
      const CodeGen::ObjectLayout &l = codegen_.objectLayouts().objectLayout(type);
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
  // Making a layout may make more: by index, as the map grows.
  std::vector<std::pair<const Type *, llvm::Constant *>> layouts;
  for (const auto &[key, descriptor] : codegen_.layoutDescriptors_) {
    layouts.push_back({key.first, descriptor});
  }
  for (const auto &[type, descriptor] : layouts) {
    if (type->isObject()) {
      add(descriptor, objectPublish(type));
    }
  }
  for (const auto &[cls, descriptor] : codegen_.classDescriptors_) {
    add(descriptor, objectPublish(cls->type));
  }
  for (const auto &[key, layout] : codegen_.envLayouts_) {
    std::vector<std::pair<const VarSymbol *, unsigned>> captures;
    for (size_t i = 0; i < layout.fields.size(); ++i) {
      if (!codegen_.uncounted_.count({key, layout.fields[i]})) {
        captures.push_back({layout.fields[i], unsigned(i + 2)});
      }
    }
    add(layout.descriptor, publishFunction(nullptr, layout.type, captures, {}, "env"));
  }
  for (const auto &[type, drop] : codegen_.boxDrops_) {
    if (!type->isManaged()) {
      continue;
    }
    llvm::Function *publish = publishFunction(nullptr, codegen_.envLowerer().boxType(type), {}, {{2u, type}}, "box");
    if (llvm::Constant *descriptor = codegen_.dropDescriptors_.lookup(drop)) {
      add(descriptor, publish);
    }
    if (llvm::Constant *descriptor = codegen_.memberDescriptors_.lookup(drop)) {
      add(descriptor, publish);
    }
  }
  for (const CodeGen::PublishedObject &o : codegen_.publishedObjects_) {
    add(o.descriptor, publishFunction(nullptr, o.type, {}, o.values, "other"));
  }
  auto *arrayType = llvm::ArrayType::get(entryType, entries.size());
  auto *table = new llvm::GlobalVariable(
      *codegen_.module_, arrayType, /*isConstant=*/true, llvm::GlobalValue::PrivateLinkage,
      llvm::ConstantArray::get(arrayType, entries), "tz.publish.table");
  llvm::IRBuilder<> b(init->getNextNode());
  b.CreateCall(
      codegen_.processLowerer().processFunction("tz_publish_init", codegen_.builder_.getVoidTy(), {ptr, codegen_.builder_.getInt32Ty()}),
      {table, b.getInt32(uint32_t(entries.size()))});
}

void PublishLowerer::pinValue(llvm::Value *value, const Type *type) {
  if (!codegen_.memoryAnalysis().mayHoldMember(type) || isa<llvm::Constant>(value)) {
    return;
  }
  llvm::Type *ptr = codegen_.builder_.getPtrTy();
  codegen_.builder_.CreateCall(codegen_.groupLowerer().groupFunction("tz_group_pin_value", codegen_.builder_.getVoidTy(), {ptr, ptr}),
                      {codegen_.typeDescriptors().typeDescriptor(type), spillHere(codegen_.builder_, value, "pinned")});
}

void PublishLowerer::endValue(llvm::Value *value, const Type *type) {
  if (!codegen_.memoryAnalysis().mayHoldMember(type)) {
    codegen_.refCounts().releaseValue(value, type);
    return;
  }
  llvm::Type *ptr = codegen_.builder_.getPtrTy();
  codegen_.builder_.CreateCall(codegen_.groupLowerer().groupFunction("tz_group_end_value", codegen_.builder_.getVoidTy(), {ptr, ptr}),
                      {codegen_.typeDescriptors().typeDescriptor(type), spillHere(codegen_.builder_, value, "ended")});
}

} // namespace tz
