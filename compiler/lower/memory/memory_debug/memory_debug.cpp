#include "compiler/lower/memory/memory_debug/memory_debug.h"
#include "compiler/lower/types/optional_lowerer/optional_lowerer.h"
#include "compiler/lower/values/string_lowerer/string_lowerer.h"
#include "compiler/lower/layouts/type_descriptors/type_descriptors.h"
#include "compiler/lower/functions/closure_lowerer/closure_lowerer.h"
#include "compiler/lower/functions/env_lowerer/env_lowerer.h"
#include "compiler/lower/memory/memory_analysis/memory_analysis.h"
#include "compiler/lower/memory/group_lowerer/group_lowerer.h"
#include "compiler/lower/layouts/object_layouts/object_layouts.h"
#include "compiler/lower/errors/unwind_lowerer/unwind_lowerer.h"
#include "compiler/lower/types/union_layout/union_layout.h"
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

// How CodeGen walks the references in a value for the group functions: the
// reference itself, or the environment of a function; anything else goes to
// the runtime by its descriptor.
enum class RefShape { None, Pointer, Function, Optional, Other };

} // namespace

MemoryDebug::MemoryDebug(CodeGen &codegen) : codegen_(codegen) {}

void MemoryDebug::checkOwned(const Type *type, llvm::StringRef name, llvm::Value *value,
                         const Type *fieldType) {
  if (!codegen_.debugMemory_) {
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
  const Type *owner = held(codegen_.ownedFields_);
  // The function in an owned object's field: only that object holds it.
  bool callback = !owner && (owner = held(codegen_.backRefFields_)) != nullptr;
  if (!owner) {
    return;
  }
  llvm::Value *object = value;
  if (fieldType->isOptional() && !UnionLayout::zeroIsUndefined(fieldType->inner())) {
    object = codegen_.optionalLowerer().optionalValue(value, fieldType);
  }
  if (callback || fieldType->nonOptional()->isFunction()) {
    object = codegen_.builder_.CreateExtractValue(object, 1);
  }
  std::string text = owner->name().str() + "." + name.str();
  llvm::Type *ptr = codegen_.builder_.getPtrTy();
  codegen_.builder_.CreateCall(codegen_.groupLowerer().groupFunction("tz_group_check_owned", codegen_.builder_.getVoidTy(),
                                    {ptr, ptr, ptr, codegen_.builder_.getInt32Ty()}),
                      {object, codegen_.unwindLowerer().frameDescriptor(codegen_.loc_), codegen_.stringLowerer().utf8Constant(text),
                       codegen_.builder_.getInt32(uint32_t(text.size()))});
}

// void trace(ptr object, ptr walk): each field that may hold a member, and
// the variables captured after them, to the walk.
llvm::Function *
MemoryDebug::traceFunction(const Type *type, llvm::StructType *layout,
                       llvm::ArrayRef<std::pair<const VarSymbol *, unsigned>> captures,
                       llvm::StringRef name) {
  llvm::Type *ptr = codegen_.builder_.getPtrTy();
  auto *fnType = llvm::FunctionType::get(codegen_.builder_.getVoidTy(), {ptr, ptr}, false);
  auto *fn = llvm::Function::Create(fnType, llvm::GlobalValue::InternalLinkage, "tz.trace." + name,
                                    codegen_.module_.get());
  fn->addFnAttr(llvm::Attribute::NoUnwind);
  llvm::IRBuilder<> b(llvm::BasicBlock::Create(codegen_.ctx_, "entry", fn));
  llvm::Value *object = fn->getArg(0), *walk = fn->getArg(1);
  llvm::FunctionCallee value =
      codegen_.groupLowerer().groupFunction("tz_group_trace_value", codegen_.builder_.getVoidTy(), {ptr, ptr, ptr, ptr});
  llvm::FunctionCallee ref = codegen_.groupLowerer().groupFunction("tz_group_trace_ref", codegen_.builder_.getVoidTy(),
                                           {ptr, ptr, ptr, codegen_.builder_.getInt32Ty()});
  auto one = [&](const Type *t, unsigned element, llvm::StringRef field) {
    if (!codegen_.memoryAnalysis().mayHoldMember(t)) {
      return;
    }
    b.CreateCall(value, {walk, codegen_.typeDescriptors().typeDescriptor(t), b.CreateStructGEP(layout, object, element),
                         codegen_.stringLowerer().utf8Constant(field)});
  };
  if (type) {
    const CodeGen::ObjectLayout &l = codegen_.objectLayouts().objectLayout(type);
    for (unsigned i = 0; i < type->fields().size(); ++i) {
      one(type->fields()[i].type, l.elements[i], type->fields()[i].name);
    }
  }
  for (auto [var, element] : captures) {
    if (var->boxed()) {
      b.CreateCall(ref, {walk, b.CreateLoad(ptr, b.CreateStructGEP(layout, object, element)),
                         codegen_.stringLowerer().utf8Constant(var->name), b.getInt32(uint32_t(var->name.size()))});
    } else {
      one(ClosureLowerer::typeOf(var), element, var->name);
    }
  }
  b.CreateRetVoid();

  return fn;
}

void MemoryDebug::finishGroupDebug(llvm::Instruction *init) {
  if (!codegen_.debugMemory_ || !codegen_.hasCycles_) {
    return;
  }
  llvm::Type *ptr = codegen_.builder_.getPtrTy();
  std::vector<llvm::Constant *> entries;
  auto *entryType = llvm::StructType::get(codegen_.ctx_, {ptr, ptr});
  auto add = [&](llvm::Constant *descriptor, llvm::Function *trace) {
    entries.push_back(llvm::ConstantStruct::get(entryType, {descriptor, trace}));
  };
  std::map<const Type *, llvm::Function *> traces;
  auto objectTrace = [&](const Type *type) {
    llvm::Function *&trace = traces[type];
    if (!trace) {
      const CodeGen::ObjectLayout &l = codegen_.objectLayouts().objectLayout(type);
      std::vector<std::pair<const VarSymbol *, unsigned>> captures;
      if (const ClassDecl *cls = type->classDecl(); cls && cls->group) {
        for (size_t i = 0; i < cls->group->captures.size(); ++i) {
          captures.push_back({cls->group->captures[i], unsigned(l.captures + i)});
        }
      }
      trace = traceFunction(type, l.type, captures, "object");
    }

    return trace;
  };
  for (const auto &[key, descriptor] : codegen_.layoutDescriptors_) {
    if (codegen_.isCyclic(key.first)) {
      add(descriptor, objectTrace(key.first));
    }
  }
  for (const auto &[cls, descriptor] : codegen_.classDescriptors_) {
    if (codegen_.isCyclic(cls->type)) {
      add(descriptor, objectTrace(cls->type));
    }
  }
  for (const auto &[key, layout] : codegen_.envLayouts_) {
    if (!layout.member) {
      continue;
    }
    std::vector<std::pair<const VarSymbol *, unsigned>> captures;
    for (size_t i = 0; i < layout.fields.size(); ++i) {
      if (!codegen_.uncounted_.count({key, layout.fields[i]})) {
        captures.push_back({layout.fields[i], unsigned(i + 2)});
      }
    }
    add(layout.descriptor, traceFunction(nullptr, layout.type, captures, "env"));
  }
  for (const auto &[descriptor, type] : codegen_.boxDescriptors_) {
    llvm::Function *trace = traceFunction(nullptr, codegen_.envLowerer().boxType(type), {}, "box");
    llvm::IRBuilder<> b(&trace->getEntryBlock(), trace->getEntryBlock().begin());
    if (codegen_.memoryAnalysis().mayHoldMember(type)) {
      b.CreateCall(
          codegen_.groupLowerer().groupFunction("tz_group_trace_value", codegen_.builder_.getVoidTy(), {ptr, ptr, ptr, ptr}),
          {trace->getArg(1), codegen_.typeDescriptors().typeDescriptor(type),
           b.CreateStructGEP(codegen_.envLowerer().boxType(type), trace->getArg(0), 2), codegen_.stringLowerer().utf8Constant("value")});
    }
    add(descriptor, trace);
  }
  auto *arrayType = llvm::ArrayType::get(entryType, entries.size());
  auto *table = new llvm::GlobalVariable(
      *codegen_.module_, arrayType, /*isConstant=*/true, llvm::GlobalValue::PrivateLinkage,
      llvm::ConstantArray::get(arrayType, entries), "tz.group.traces");
  llvm::IRBuilder<> b(init->getNextNode());
  b.CreateCall(
      codegen_.groupLowerer().groupFunction("tz_group_debug_init", codegen_.builder_.getVoidTy(), {ptr, codegen_.builder_.getInt32Ty()}),
      {table, b.getInt32(uint32_t(entries.size()))});
}

} // namespace tz
