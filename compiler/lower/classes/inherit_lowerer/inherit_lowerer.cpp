#include "compiler/lower/classes/inherit_lowerer/inherit_lowerer.h"
#include "compiler/lower/module_builder/module_builder.h"
#include "compiler/lower/function_builder/function_builder.h"
#include "compiler/lower/memory/ref_counts/ref_counts.h"
#include "compiler/lower/types/optional_lowerer/optional_lowerer.h"
#include "compiler/lower/stmt/stmt_lowerer/stmt_lowerer.h"
#include "compiler/lower/expr/assign_lowerer/assign_lowerer.h"
#include "compiler/lower/values/string_lowerer/string_lowerer.h"
#include "compiler/lower/classes/class_descriptors/class_descriptors.h"
#include "compiler/lower/functions/closure_lowerer/closure_lowerer.h"
#include "compiler/lower/layouts/object_layouts/object_layouts.h"
#include "compiler/lower/errors/may_throw/may_throw.h"
#include "compiler/lower/errors/throw_lowerer/throw_lowerer.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/ADT/SmallPtrSet.h"
#include "llvm/Support/ConvertUTF.h"

using llvm::cast;
using llvm::dyn_cast;
using llvm::isa;
using llvm::BasicBlock;
using llvm::Value;

namespace tz {

namespace {

// Where a descriptor holds its tables of interfaces: after its TzLayout.
constexpr uint64_t kItablesOffset = 40;

// A load from a descriptor, which never changes.
llvm::LoadInst *invariant(llvm::LoadInst *load) {
  load->setMetadata(llvm::LLVMContext::MD_invariant_load,
                    llvm::MDNode::get(load->getContext(), {}));

  return load;
}

std::string typeName(const Type *type) {
  llvm::StringRef name = type->name();

  return name.starts_with("{") ? "object" : name.str();
}

} // namespace

InheritLowerer::InheritLowerer(CodeGen &codegen) : codegen_(codegen) {}

const Type *InheritLowerer::layoutPrefix(const Type *type) const {
  for (const Type *base : type->bases()) {
    if (!base->fields().empty()) {
      return base;
    }
  }

  return nullptr;
}

void InheritLowerer::prepareDispatch(SourceFile &file) {
  // The types up each declared chain; the interfaces in the order found.
  std::vector<const Type *> order;
  auto implement = [&](const Type *type, const Type *iface) {
    codegen_.withSubtypes_.insert(iface);
    if (iface->isClass()) {
      return false;
    }
    auto [it, inserted] = codegen_.interfaces_.try_emplace(iface);
    if (inserted) {
      order.push_back(iface);
      it->second.implementers.push_back(iface);
    }
    if (llvm::is_contained(it->second.implementers, type)) {
      return false;
    }
    it->second.implementers.push_back(type);

    return true;
  };
  for (const Type *type : file.subtypes) {
    if (mentionsParam(type)) {
      continue; // of generic code, which is emitted only as its instances
    }
    std::vector<const Type *> queue(type->bases().begin(), type->bases().end());
    llvm::SmallPtrSet<const Type *, 8> seen;
    for (size_t i = 0; i < queue.size(); ++i) {
      const Type *base = queue[i];
      if (!seen.insert(base).second) {
        continue;
      }
      queue.insert(queue.end(), base->bases().begin(), base->bases().end());
      implement(type, base);
    }
  }
  // A type with read-only fields that objects of another are seen as is
  // one too, of what the other's objects may be.
  for (bool changed = true; changed;) {
    changed = false;
    for (auto [from, to] : file.views) {
      if (mentionsParam(from) || mentionsParam(to)) {
        continue;
      }
      changed |= implement(from, to);
      if (auto found = codegen_.interfaces_.find(from); found != codegen_.interfaces_.end()) {
        std::vector<const Type *> others = found->second.implementers;
        for (const Type *other : others) {
          changed |= implement(other, to);
        }
      }
    }
  }

  const llvm::DataLayout &data = codegen_.module_->getDataLayout();
  auto offsetOf = [&](const Type *type, unsigned field) {
    const CodeGen::ObjectLayout &layout = codegen_.objectLayouts().objectLayout(type);

    return data.getStructLayout(layout.type)->getElementOffset(layout.elements[field]);
  };
  for (const Type *iface : order) {
    CodeGen::InterfaceInfo &info = codegen_.interfaces_[iface];
    std::vector<llvm::Type *> entries;
    llvm::ArrayRef<Field> fields = iface->fields();
    info.members.resize(fields.size());
    for (unsigned k = 0; k < fields.size(); ++k) {
      const Field &f = fields[k];
      CodeGen::InterfaceMember &member = info.members[k];
      // A method of some class that implements it.
      bool callable = f.type->isFunction() && llvm::any_of(info.implementers, [&](const Type *t) {
                        return t->isClass() && !t->field(f.name);
                      });
      if (callable) {
        member.entry = int(entries.size());
        member.callable = true;
        entries.push_back(codegen_.builder_.getPtrTy()); // the code
        entries.push_back(codegen_.builder_.getPtrTy()); // its info
        continue;
      }
      uint64_t offset = offsetOf(iface, k);
      if (llvm::all_of(info.implementers, [&](const Type *t) {
            return offsetOf(t, ObjectLayouts::fieldIndex(t, f.name)) == offset;
          })) {
        continue;
      }
      member.entry = int(entries.size());
      entries.push_back(codegen_.builder_.getInt64Ty()); // the offset
    }
    // A method with several signatures is a method of classes only.
    for (const Overload &overload : iface->overloads()) {
      std::vector<CodeGen::InterfaceMember> &signatures = info.overloads.emplace_back();
      for (size_t i = 0; i < overload.signatures.size(); ++i) {
        signatures.push_back({int(entries.size()), true});
        entries.push_back(codegen_.builder_.getPtrTy());
        entries.push_back(codegen_.builder_.getPtrTy());
      }
    }
    info.dispatch = !entries.empty();
    info.table = llvm::StructType::get(codegen_.ctx_, entries);
  }

  // The numbers: interfaces that some type implements together differ.
  std::vector<const Type *> numbered;
  for (const Type *iface : order) {
    CodeGen::InterfaceInfo &info = codegen_.interfaces_[iface];
    if (!info.dispatch) {
      continue;
    }
    llvm::SmallVector<unsigned, 4> taken;
    for (const Type *other : numbered) {
      const CodeGen::InterfaceInfo &o = codegen_.interfaces_[other];
      if (llvm::any_of(info.implementers,
                       [&](const Type *t) { return llvm::is_contained(o.implementers, t); })) {
        taken.push_back(o.color);
      }
    }
    while (llvm::is_contained(taken, info.color)) {
      ++info.color;
    }
    colors_ = std::max(colors_, info.color + 1);
    numbered.push_back(iface);
    for (const Type *t : info.implementers) {
      tablesOf_[t].push_back(iface);
    }
  }
}

const CodeGen::InterfaceMember *InheritLowerer::dispatchedMember(const Type *type, llvm::StringRef name,
                                                          int signature) {
  if (!type->isObject()) {
    return nullptr;
  }
  auto found = codegen_.interfaces_.find(type);
  if (found == codegen_.interfaces_.end() || !found->second.dispatch) {
    return nullptr;
  }
  if (signature >= 0) {
    llvm::ArrayRef<Overload> overloads = type->overloads();
    for (size_t i = 0; i < overloads.size(); ++i) {
      if (overloads[i].name == name) {
        return &found->second.overloads[i][size_t(signature)];
      }
    }
    return nullptr;
  }
  if (!type->field(name)) {
    return nullptr;
  }
  const CodeGen::InterfaceMember &member = found->second.members[ObjectLayouts::fieldIndex(type, name)];

  return member.entry >= 0 ? &member : nullptr;
}

llvm::Value *InheritLowerer::tableEntry(llvm::Value *object, const Type *iface, unsigned entry) {
  const CodeGen::InterfaceInfo &info = codegen_.interfaces_.find(iface)->second;
  llvm::Type *ptr = codegen_.builder_.getPtrTy();
  llvm::Value *descriptor = codegen_.builder_.CreateLoad(
      ptr, codegen_.builder_.CreateConstInBoundsGEP1_64(codegen_.builder_.getInt8Ty(), object, 8), "descriptor");
  llvm::Value *itables = invariant(codegen_.builder_.CreateLoad(
      ptr, codegen_.builder_.CreateConstInBoundsGEP1_64(codegen_.builder_.getInt8Ty(), descriptor, kItablesOffset),
      "itables"));
  llvm::Value *table = invariant(codegen_.builder_.CreateLoad(
      ptr, codegen_.builder_.CreateConstInBoundsGEP1_64(ptr, itables, info.color), "itable"));

  return codegen_.builder_.CreateStructGEP(info.table, table, entry);
}

llvm::Value *InheritLowerer::vtableEntry(llvm::Value *object, int slot, bool info) {
  llvm::Type *ptr = codegen_.builder_.getPtrTy();
  llvm::Value *descriptor = codegen_.builder_.CreateLoad(
      ptr, codegen_.builder_.CreateConstInBoundsGEP1_64(codegen_.builder_.getInt8Ty(), object, 8), "descriptor");
  uint64_t offset = codegen_.kVtableOffset + 16 * uint64_t(slot) + (info ? 8 : 0);

  return invariant(codegen_.builder_.CreateLoad(
      ptr, codegen_.builder_.CreateConstInBoundsGEP1_64(codegen_.builder_.getInt8Ty(), descriptor, offset),
      info ? "info" : "code"));
}

llvm::Constant *InheritLowerer::itablesOf(const Type *type) {
  llvm::Constant *null = llvm::ConstantPointerNull::get(codegen_.builder_.getPtrTy());
  auto found = tablesOf_.find(type);
  if (found == tablesOf_.end()) {
    return null;
  }
  llvm::Constant *&global = itables_[type];
  if (global) {
    return global;
  }
  std::vector<llvm::Constant *> tables(colors_, null);
  for (const Type *iface : found->second) {
    tables[codegen_.interfaces_[iface].color] = itable(type, iface);
  }
  auto *arrayType = llvm::ArrayType::get(codegen_.builder_.getPtrTy(), colors_);
  auto *array = new llvm::GlobalVariable(
      *codegen_.module_, arrayType, /*isConstant=*/true, llvm::GlobalValue::PrivateLinkage,
      llvm::ConstantArray::get(arrayType, tables), "tz.itables." + typeName(type));
  array->setUnnamedAddr(llvm::GlobalValue::UnnamedAddr::Global);
  global = array;

  return global;
}

llvm::Constant *InheritLowerer::itable(const Type *type, const Type *iface) {
  llvm::Constant *&global = itableConstants_[{type, iface}];
  if (global) {
    return global;
  }
  const CodeGen::InterfaceInfo &info = codegen_.interfaces_[iface];
  const llvm::DataLayout &data = codegen_.module_->getDataLayout();
  llvm::Constant *null = llvm::ConstantPointerNull::get(codegen_.builder_.getPtrTy());
  std::vector<llvm::Constant *> entries;
  llvm::ArrayRef<Field> fields = iface->fields();
  for (unsigned k = 0; k < fields.size(); ++k) {
    const CodeGen::InterfaceMember &member = info.members[k];
    if (member.entry < 0) {
      continue;
    }
    const Field &f = fields[k];
    if (!member.callable) {
      const CodeGen::ObjectLayout &layout = codegen_.objectLayouts().objectLayout(type);
      uint64_t offset = data.getStructLayout(layout.type)
                            ->getElementOffset(layout.elements[ObjectLayouts::fieldIndex(type, f.name)]);
      entries.push_back(codegen_.builder_.getInt64(offset));
      continue;
    }
    if (const ClassDecl *cls = type->classDecl(); cls && !type->field(f.name)) {
      // A class with the method: an abstract class has no objects.
      FunctionDecl *fn = implementingMethod(cls, f.name, f.type);
      entries.push_back(fn ? codegen_.classDescriptors().methodCode(fn) : null);
      entries.push_back(fn ? codegen_.closureLowerer().functionInfo(fn) : null);
      continue;
    }
    auto [code, codeInfo] = fieldStub(type, f);
    entries.push_back(code);
    entries.push_back(codeInfo);
  }
  // The variant of a class for each signature; a type that is not a class
  // has no objects with such methods.
  for (const Overload &overload : iface->overloads()) {
    for (const Type *signature : overload.signatures) {
      const ClassDecl *cls = type->classDecl();
      FunctionDecl *fn = cls ? implementingMethod(cls, overload.name, signature) : nullptr;
      entries.push_back(fn ? codegen_.classDescriptors().methodCode(fn) : null);
      entries.push_back(fn ? codegen_.closureLowerer().functionInfo(fn) : null);
    }
  }
  auto *table = new llvm::GlobalVariable(*codegen_.module_, info.table, /*isConstant=*/true,
                                         llvm::GlobalValue::PrivateLinkage,
                                         llvm::ConstantStruct::get(info.table, entries),
                                         "tz.itable." + typeName(type) + "." + typeName(iface));
  table->setUnnamedAddr(llvm::GlobalValue::UnnamedAddr::Global);
  global = table;

  return global;
}

FunctionDecl *InheritLowerer::implementingMethod(const ClassDecl *cls, llvm::StringRef name,
                                          const Type *fnType) {
  for (const ClassDecl *c = cls; c; c = c->parent) {
    std::vector<FunctionDecl *> methods;
    for (FunctionDecl *fn : c->methods) {
      if (!fn->isStatic) {
        methods.push_back(fn);
      }
    }
    for (const ClassField &f : c->fields) {
      if (f.method) {
        methods.push_back(f.method);
      }
    }
    for (FunctionDecl *fn : methods) {
      if (fn->name != name || fn->isAbstract || !fn->sym) {
        continue;
      }
      const Type *type = ClosureLowerer::functionTypeOf(fn->sym);
      if (type->params() == fnType->params() && type->minArgs() == fnType->minArgs() &&
          type->hasRest() == fnType->hasRest()) {
        return fn;
      }
    }
  }

  return nullptr;
}

// The function in the field is kept alive while it runs: the call may replace
// what the field holds.
std::pair<llvm::Function *, llvm::Constant *> InheritLowerer::fieldStub(const Type *type,
                                                                 const Field &member) {
  auto &stub = fieldStubs_[{type, member.name}];
  if (stub.first) {
    return stub;
  }
  const Type *fnType = member.type;
  auto *code =
      llvm::Function::Create(codegen_.closureLowerer().closureFnType(fnType), llvm::GlobalValue::InternalLinkage,
                             "tz." + typeName(type) + "." + member.name + ".stub", codegen_.module_.get());
  codegen_.functionBuilder().setFunctionAttributes(code);
  llvm::IRBuilderBase::InsertPoint point = codegen_.builder_.saveIP();
  codegen_.builder_.SetInsertPoint(llvm::BasicBlock::Create(codegen_.ctx_, "entry", code));
  llvm::Value *fn = codegen_.builder_.CreateLoad(
      codegen_.fnTy_, codegen_.objectLayouts().fieldAddress(code->getArg(0), type, member.name, /*exact=*/true), member.name);
  codegen_.refCounts().retainValue(fn, fnType);
  std::vector<llvm::Value *> args;
  for (unsigned i = 1; i < code->arg_size(); ++i) {
    args.push_back(code->getArg(i));
  }
  llvm::Value *result = codegen_.closureLowerer().callFunctionValue(fn, fnType, args);
  codegen_.refCounts().releaseValue(fn, fnType);
  if (result) {
    codegen_.builder_.CreateRet(result);
  } else {
    codegen_.builder_.CreateRetVoid();
  }
  codegen_.builder_.restoreIP(point);
  llvm::SmallVector<llvm::UTF16, 16> units;
  llvm::convertUTF8ToUTF16String(member.name, units);
  auto *info = new llvm::GlobalVariable(
      *codegen_.module_, codegen_.fnInfoTy_, /*isConstant=*/true, llvm::GlobalValue::PrivateLinkage,
      llvm::ConstantStruct::get(codegen_.fnInfoTy_,
                                {code, codegen_.stringLowerer().stringLiteral(std::u16string(units.begin(), units.end()))}),
      "tz.fninfo." + member.name);
  stub = {code, info};

  return stub;
}

// `shape.area()` through an interface: the code the object's table gives,
// called with the object, as a method is.
llvm::Value *InheritLowerer::emitInterfaceCall(CallExpr *call) {
  auto *member = cast<MemberExpr>(call->callee);
  const Type *iface = member->object->type;
  const CodeGen::InterfaceMember *method = dispatchedMember(iface, member->name, call->signature);
  const Type *fnType = member->type;
  if (!method) {
    // A method with several signatures of an interface that no class
    // implements: no object can get here.
    for (Expr *arg : call->args) {
      codegen_.stmtLowerer().emitDiscarded(arg);
    }
    codegen_.builder_.CreateCall(
        llvm::Intrinsic::getOrInsertDeclaration(codegen_.module_.get(), llvm::Intrinsic::trap));
    return fnType->ret()->isVoid() ? nullptr : llvm::PoisonValue::get(codegen_.moduleBuilder().lower(fnType->ret()));
  }
  bool borrow = staysDuringCall(member->object) && llvm::all_of(call->args, AssignLowerer::cannotAssign);
  CodeGen::Operand self = codegen_.refCounts().emitOperand(member->object, borrow);
  // The object waits while an argument that may throw is computed.
  codegen_.throwLowerer().holdOperand(self, llvm::any_of(call->args, [&](Expr *arg) { return codegen_.mayThrow().exprThrows(arg); }));
  std::vector<llvm::Value *> values = codegen_.throwLowerer().emitHeldValues(call->args);
  codegen_.throwLowerer().takeOperand(self);
  std::vector<llvm::Value *> args{self.value};
  args.insert(args.end(), values.begin(), values.end());
  for (size_t i = call->args.size(); i < fnType->params().size(); ++i) {
    args.push_back(codegen_.optionalLowerer().undefinedValue(fnType->params()[i]));
  }
  llvm::Value *code = invariant(codegen_.builder_.CreateLoad(
      codegen_.builder_.getPtrTy(), tableEntry(self.value, iface, unsigned(method->entry)), "code"));
  llvm::CallInst *result = codegen_.builder_.CreateCall(codegen_.closureLowerer().closureFnType(fnType), code, args);
  // The object goes before what the method threw does.
  codegen_.refCounts().dropOperand(self);
  codegen_.emitThrowCheckIf(codegen_.typeThrows(fnType));

  return result->getType()->isVoidTy() ? nullptr : result;
}

} // namespace tz
