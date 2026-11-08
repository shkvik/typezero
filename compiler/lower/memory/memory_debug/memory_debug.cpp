#include "compiler/lower/memory/memory_debug/memory_debug.h"
#include "compiler/lower/functions/closure_lowerer/closure_lowerer.h"
#include "compiler/lower/functions/env_lowerer/env_lowerer.h"
#include "compiler/lower/layouts/object_layouts/object_layouts.h"
#include "compiler/lower/layouts/type_descriptors/type_descriptors.h"
#include "compiler/lower/memory/group_lowerer/group_lowerer.h"
#include "compiler/lower/places/place_lowerer/place_lowerer.h"
#include "compiler/lower/types/optional_lowerer/optional_lowerer.h"
#include "compiler/lower/types/union_layout/union_layout.h"
#include "compiler/lower/values/string_lowerer/string_lowerer.h"
#include "compiler/memory/memory_plan/memory_plan.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/IR/MDBuilder.h"
#include <map>
#include <set>


namespace tz {

namespace {

enum class RefShape { None, Pointer, Function, Optional, Other };

}

MemoryDebug::MemoryDebug(Lowering &owner)
    : lowering(owner) {}

void MemoryDebug::checkOwned(const Type *type, llvm::StringRef name, llvm::Value *value,
                             const Type *fieldType) {
  if (!this->lowering.debugMemory) {
    return;
  }
  auto held = [&](const std::set<std::pair<const Type *, std::string>> &fields) -> const Type * {
    for (const auto &[owner, field] : fields) {
      if (field == name && type->isSubtypeOf(owner)) {
        return owner;
      }
    }

    return nullptr;
  };
  const Type *owner = held(this->lowering.memory().ownedFields);
  bool callback = !owner && (owner = held(this->lowering.memory().backRefFields)) != nullptr;
  if (!owner) {
    return;
  }
  llvm::Value *object = value;
  if (fieldType->isOptional() && !UnionLayout::zeroIsUndefined(fieldType->inner())) {
    object = this->lowering.optionalLowerer().optionalValue(value, fieldType);
  }
  if (callback || fieldType->nonOptional()->isFunction()) {
    object = this->lowering.builder.CreateExtractValue(object, 1);
  }
  std::string text = owner->name().str() + "." + name.str();
  llvm::Type *ptr = this->lowering.builder.getPtrTy();
  this->lowering.builder.CreateCall(
      this->lowering.groupLowerer().groupFunction(
          "tz_group_check_owned", this->lowering.builder.getVoidTy(),
          {ptr, ptr, ptr, this->lowering.builder.getInt32Ty()}),
      {object, this->lowering.placeLowerer().frameDescriptor(this->lowering.loc_),
       this->lowering.stringLowerer().utf8Constant(text),
       this->lowering.builder.getInt32(uint32_t(text.size()))});
}

llvm::Function *
MemoryDebug::traceFunction(const Type *type, llvm::StructType *layout,
                           llvm::ArrayRef<std::pair<const VarSymbol *, unsigned>> captures,
                           llvm::StringRef name) {
  llvm::Type *ptr = this->lowering.builder.getPtrTy();
  auto *fnType = llvm::FunctionType::get(this->lowering.builder.getVoidTy(), {ptr, ptr}, false);
  auto *fn = llvm::Function::Create(fnType, llvm::GlobalValue::InternalLinkage, "tz.trace." + name,
                                    this->lowering.module_.get());
  fn->addFnAttr(llvm::Attribute::NoUnwind);
  llvm::IRBuilder<> b(llvm::BasicBlock::Create(this->lowering.ctx, "entry", fn));
  llvm::Value *object = fn->getArg(0);
  llvm::Value *walk = fn->getArg(1);
  llvm::FunctionCallee value = this->lowering.groupLowerer().groupFunction(
      "tz_group_trace_value", this->lowering.builder.getVoidTy(), {ptr, ptr, ptr, ptr});
  llvm::FunctionCallee ref = this->lowering.groupLowerer().groupFunction(
      "tz_group_trace_ref", this->lowering.builder.getVoidTy(),
      {ptr, ptr, ptr, this->lowering.builder.getInt32Ty()});
  auto one = [&](const Type *t, unsigned element, llvm::StringRef field) {
    if (!this->lowering.memory().mayCarryMember(t)) {
      return;
    }
    b.CreateCall(value, {walk, this->lowering.typeDescriptors().typeDescriptor(t),
                         b.CreateStructGEP(layout, object, element),
                         this->lowering.stringLowerer().utf8Constant(field)});
  };
  if (type) {
    const Lowering::ObjectLayout &l = this->lowering.objectLayouts().objectLayout(type);
    for (unsigned i = 0; i < type->fields().size(); ++i) {
      one(type->fields()[i].type, l.elements[i], type->fields()[i].name);
    }
  }
  for (auto [var, element] : captures) {
    if (var->boxed()) {
      b.CreateCall(ref, {walk, b.CreateLoad(ptr, b.CreateStructGEP(layout, object, element)),
                         this->lowering.stringLowerer().utf8Constant(var->name),
                         b.getInt32(uint32_t(var->name.size()))});
    } else {
      one(ClosureLowerer::typeOf(var), element, var->name);
    }
  }
  b.CreateRetVoid();

  return fn;
}

void MemoryDebug::finishGroupDebug(llvm::Instruction *init) {
  if (!this->lowering.debugMemory || !this->lowering.memory().hasCycles) {
    return;
  }
  llvm::Type *ptr = this->lowering.builder.getPtrTy();
  std::vector<llvm::Constant *> entries;
  auto *entryType = llvm::StructType::get(this->lowering.ctx, {ptr, ptr});
  auto add = [&](llvm::Constant *descriptor, llvm::Function *trace) {
    entries.push_back(llvm::ConstantStruct::get(entryType, {descriptor, trace}));
  };
  std::map<const Type *, llvm::Function *> traces;
  auto objectTrace = [&](const Type *type) {
    llvm::Function *&trace = traces[type];
    if (!trace) {
      const Lowering::ObjectLayout &l = this->lowering.objectLayouts().objectLayout(type);
      std::vector<std::pair<const VarSymbol *, unsigned>> captures;
      const ClassDecl *cls = type->classDecl();
      if (cls && cls->group) {
        for (size_t i = 0; i < cls->group->captures.size(); ++i) {
          captures.push_back({cls->group->captures[i], unsigned(l.captures + i)});
        }
      }
      trace = this->traceFunction(type, l.type, captures, "object");
    }

    return trace;
  };
  for (const auto &[key, descriptor] : this->lowering.layoutDescriptors) {
    if (this->lowering.isCyclic(key.first)) {
      add(descriptor, objectTrace(key.first));
    }
  }
  for (const auto &[cls, descriptor] : this->lowering.classDescriptors_) {
    if (this->lowering.isCyclic(cls->type)) {
      add(descriptor, objectTrace(cls->type));
    }
  }
  for (const auto &[key, layout] : this->lowering.envLayouts) {
    if (!layout.member) {
      continue;
    }
    std::vector<std::pair<const VarSymbol *, unsigned>> captures;
    for (size_t i = 0; i < layout.fields.size(); ++i) {
      if (!this->lowering.memory().uncounted.count({key, layout.fields[i]})) {
        captures.push_back({layout.fields[i], unsigned(i + 2)});
      }
    }
    add(layout.descriptor, this->traceFunction(nullptr, layout.type, captures, "env"));
  }
  for (const auto &[descriptor, type] : this->lowering.boxDescriptors) {
    llvm::Function *trace =
        this->traceFunction(nullptr, this->lowering.envLowerer().boxType(type), {}, "box");
    llvm::IRBuilder<> b(&trace->getEntryBlock(), trace->getEntryBlock().begin());
    if (this->lowering.memory().mayCarryMember(type)) {
      b.CreateCall(
          this->lowering.groupLowerer().groupFunction(
              "tz_group_trace_value", this->lowering.builder.getVoidTy(), {ptr, ptr, ptr, ptr}),
          {trace->getArg(1), this->lowering.typeDescriptors().typeDescriptor(type),
           b.CreateStructGEP(this->lowering.envLowerer().boxType(type), trace->getArg(0), 2),
           this->lowering.stringLowerer().utf8Constant("value")});
    }
    add(descriptor, trace);
  }
  auto *arrayType = llvm::ArrayType::get(entryType, entries.size());
  auto *table = new llvm::GlobalVariable(
      *this->lowering.module_, arrayType, true, llvm::GlobalValue::PrivateLinkage,
      llvm::ConstantArray::get(arrayType, entries), "tz.group.traces");
  llvm::IRBuilder<> b(init->getNextNode());
  b.CreateCall(this->lowering.groupLowerer().groupFunction(
                   "tz_group_debug_init", this->lowering.builder.getVoidTy(),
                   {ptr, this->lowering.builder.getInt32Ty()}),
               {table, b.getInt32(uint32_t(entries.size()))});
}

}
