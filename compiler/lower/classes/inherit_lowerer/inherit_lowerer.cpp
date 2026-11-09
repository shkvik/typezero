#include "compiler/lower/classes/inherit_lowerer/inherit_lowerer.h"
#include "compiler/lower/classes/class_descriptors/class_descriptors.h"
#include "compiler/lower/classes/class_lowerer/class_lowerer.h"
#include "compiler/lower/expr/assign_lowerer/assign_lowerer.h"
#include "compiler/lower/expr/expr_lowerer/expr_lowerer.h"
#include "compiler/lower/function_builder/function_builder.h"
#include "compiler/lower/functions/closure_lowerer/closure_lowerer.h"
#include "compiler/lower/layouts/object_layouts/object_layouts.h"
#include "compiler/lower/memory/ref_counts/ref_counts.h"
#include "compiler/lower/module_builder/module_builder.h"
#include "compiler/lower/stmt/stmt_lowerer/stmt_lowerer.h"
#include "compiler/lower/types/optional_lowerer/optional_lowerer.h"
#include "compiler/lower/values/string_lowerer/string_lowerer.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/ADT/SmallPtrSet.h"
#include "llvm/Support/ConvertUTF.h"

using llvm::cast;

namespace tz {

namespace {

constexpr uint64_t kItablesOffset = 40;

}

InheritLowerer::InheritLowerer(Lowering &owner)
    : lowering(owner) {}

llvm::LoadInst *InheritLowerer::invariant(llvm::LoadInst *load) {
  load->setMetadata(llvm::LLVMContext::MD_invariant_load,
                    llvm::MDNode::get(load->getContext(), {}));

  return load;
}

std::string InheritLowerer::typeName(const Type *type) {
  llvm::StringRef name = type->name();

  return name.starts_with("{") ? "object" : name.str();
}

const Type *InheritLowerer::layoutPrefix(const Type *type) const {
  for (const Type *base : type->bases()) {
    if (!base->fields().empty()) {
      return base;
    }
  }

  return nullptr;
}

void InheritLowerer::prepareDispatch(SourceFile &file) {
  std::vector<const Type *> order;
  auto implement = [&](const Type *type, const Type *iface) {
    this->lowering.withSubtypes.insert(iface);
    if (iface->isClass()) {
      return false;
    }
    auto [it, inserted] = this->lowering.interfaces.try_emplace(iface);
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
  for (const Type *type : file.inventory.subtypes) {
    if (TypeQueries::mentionsParam(type)) {
      continue;
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
  for (bool changed = true; changed;) {
    changed = false;
    for (auto [from, to] : file.inventory.views) {
      if (TypeQueries::mentionsParam(from) || TypeQueries::mentionsParam(to)) {
        continue;
      }
      changed |= implement(from, to);
      auto found = this->lowering.interfaces.find(from);
      if (found != this->lowering.interfaces.end()) {
        std::vector<const Type *> others = found->second.implementers;
        for (const Type *other : others) {
          changed |= implement(other, to);
        }
      }
    }
  }

  const llvm::DataLayout &data = this->lowering.module_->getDataLayout();
  auto offsetOf = [&](const Type *type, unsigned field) {
    const Lowering::ObjectLayout &layout = this->lowering.objectLayouts().objectLayout(type);

    return data.getStructLayout(layout.type)->getElementOffset(layout.elements[field]);
  };
  for (const Type *iface : order) {
    Lowering::InterfaceInfo &info = this->lowering.interfaces[iface];
    std::vector<llvm::Type *> entries;
    llvm::ArrayRef<Field> fields = iface->fields();
    info.members.resize(fields.size());
    for (unsigned k = 0; k < fields.size(); ++k) {
      const Field &f = fields[k];
      Lowering::InterfaceMember &member = info.members[k];
      bool callable = f.type->isFunction() && llvm::any_of(info.implementers, [&](const Type *t) {
                        return t->isClass() && !t->field(f.name);
                      });
      if (callable) {
        member.entry = int(entries.size());
        member.callable = true;
        entries.push_back(this->lowering.builder.getPtrTy());
        entries.push_back(this->lowering.builder.getPtrTy());
        continue;
      }
      uint64_t offset = offsetOf(iface, k);
      if (llvm::all_of(info.implementers, [&](const Type *t) {
            return offsetOf(t, ObjectLayouts::fieldIndex(t, f.name)) == offset;
          })) {
        continue;
      }
      member.entry = int(entries.size());
      entries.push_back(this->lowering.builder.getInt64Ty());
    }
    for (const Overload &overload : iface->overloads()) {
      std::vector<Lowering::InterfaceMember> &signatures = info.overloads.emplace_back();
      for (size_t i = 0; i < overload.signatures.size(); ++i) {
        signatures.push_back({int(entries.size()), true});
        entries.push_back(this->lowering.builder.getPtrTy());
        entries.push_back(this->lowering.builder.getPtrTy());
      }
    }
    info.dispatch = !entries.empty();
    info.table = llvm::StructType::get(this->lowering.ctx, entries);
  }

  std::vector<const Type *> numbered;
  for (const Type *iface : order) {
    Lowering::InterfaceInfo &info = this->lowering.interfaces[iface];
    if (!info.dispatch) {
      continue;
    }
    llvm::SmallVector<unsigned, 4> taken;
    for (const Type *other : numbered) {
      const Lowering::InterfaceInfo &o = this->lowering.interfaces[other];
      if (llvm::any_of(info.implementers,
                       [&](const Type *t) { return llvm::is_contained(o.implementers, t); })) {
        taken.push_back(o.color);
      }
    }
    while (llvm::is_contained(taken, info.color)) {
      ++info.color;
    }
    this->colors = std::max(this->colors, info.color + 1);
    numbered.push_back(iface);
    for (const Type *t : info.implementers) {
      this->tablesOf[t].push_back(iface);
    }
  }
}

const Lowering::InterfaceMember *
InheritLowerer::dispatchedMember(const Type *type, llvm::StringRef name, int signature) {
  if (!type->isObject()) {
    return nullptr;
  }
  auto found = this->lowering.interfaces.find(type);
  if (found == this->lowering.interfaces.end() || !found->second.dispatch) {
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
  const Lowering::InterfaceMember &member =
      found->second.members[ObjectLayouts::fieldIndex(type, name)];

  return member.entry >= 0 ? &member : nullptr;
}

llvm::Value *InheritLowerer::tableEntry(llvm::Value *object, const Type *iface, unsigned entry) {
  const Lowering::InterfaceInfo &info = this->lowering.interfaces.find(iface)->second;
  llvm::Type *ptr = this->lowering.builder.getPtrTy();
  llvm::Value *descriptor =
      this->lowering.builder.CreateLoad(ptr,
                                        this->lowering.builder.CreateConstInBoundsGEP1_64(
                                            this->lowering.builder.getInt8Ty(), object, 8),
                                        "descriptor");
  llvm::Value *tableList = InheritLowerer::invariant(this->lowering.builder.CreateLoad(
      ptr,
      this->lowering.builder.CreateConstInBoundsGEP1_64(this->lowering.builder.getInt8Ty(),
                                                        descriptor, kItablesOffset),
      "itables"));
  llvm::Value *table = InheritLowerer::invariant(this->lowering.builder.CreateLoad(
      ptr, this->lowering.builder.CreateConstInBoundsGEP1_64(ptr, tableList, info.color),
      "itable"));

  return this->lowering.builder.CreateStructGEP(info.table, table, entry);
}

llvm::Value *InheritLowerer::vtableEntry(llvm::Value *object, int slot, bool info) {
  llvm::Type *ptr = this->lowering.builder.getPtrTy();
  llvm::Value *descriptor =
      this->lowering.builder.CreateLoad(ptr,
                                        this->lowering.builder.CreateConstInBoundsGEP1_64(
                                            this->lowering.builder.getInt8Ty(), object, 8),
                                        "descriptor");
  uint64_t offset = this->lowering.kVtableOffset + 16 * uint64_t(slot) + (info ? 8 : 0);

  return InheritLowerer::invariant(
      this->lowering.builder.CreateLoad(ptr,
                                        this->lowering.builder.CreateConstInBoundsGEP1_64(
                                            this->lowering.builder.getInt8Ty(), descriptor, offset),
                                        info ? "info" : "code"));
}

llvm::Constant *InheritLowerer::itablesOf(const Type *type) {
  llvm::Constant *null = llvm::ConstantPointerNull::get(this->lowering.builder.getPtrTy());
  auto found = this->tablesOf.find(type);
  if (found == this->tablesOf.end()) {
    return null;
  }
  llvm::Constant *&global = this->itables[type];
  if (global) {
    return global;
  }
  std::vector<llvm::Constant *> tables(this->colors, null);
  for (const Type *iface : found->second) {
    tables[this->lowering.interfaces[iface].color] = this->itable(type, iface);
  }
  auto *arrayType = llvm::ArrayType::get(this->lowering.builder.getPtrTy(), this->colors);
  auto *array = new llvm::GlobalVariable(
      *this->lowering.module_, arrayType, true, llvm::GlobalValue::PrivateLinkage,
      llvm::ConstantArray::get(arrayType, tables), "tz.itables." + InheritLowerer::typeName(type));
  array->setUnnamedAddr(llvm::GlobalValue::UnnamedAddr::Global);
  global = array;

  return global;
}

llvm::Constant *InheritLowerer::itable(const Type *type, const Type *iface) {
  llvm::Constant *&global = this->itableConstants[{type, iface}];
  if (global) {
    return global;
  }
  const Lowering::InterfaceInfo &info = this->lowering.interfaces[iface];
  const llvm::DataLayout &data = this->lowering.module_->getDataLayout();
  llvm::Constant *null = llvm::ConstantPointerNull::get(this->lowering.builder.getPtrTy());
  std::vector<llvm::Constant *> entries;
  llvm::ArrayRef<Field> fields = iface->fields();
  for (unsigned k = 0; k < fields.size(); ++k) {
    const Lowering::InterfaceMember &member = info.members[k];
    if (member.entry < 0) {
      continue;
    }
    const Field &f = fields[k];
    if (!member.callable) {
      const Lowering::ObjectLayout &layout = this->lowering.objectLayouts().objectLayout(type);
      uint64_t offset =
          data.getStructLayout(layout.type)
              ->getElementOffset(layout.elements[ObjectLayouts::fieldIndex(type, f.name)]);
      entries.push_back(this->lowering.builder.getInt64(offset));
      continue;
    }
    const ClassDecl *cls = type->classDecl();
    if (cls && !type->field(f.name)) {
      FunctionDecl *fn = this->implementingMethod(cls, f.name, f.type);
      entries.push_back(fn ? this->lowering.classDescriptors().methodCode(fn) : null);
      entries.push_back(fn ? this->lowering.closureLowerer().functionInfo(fn) : null);
      continue;
    }
    auto [code, codeInfo] = this->fieldStub(type, f);
    entries.push_back(code);
    entries.push_back(codeInfo);
  }
  for (const Overload &overload : iface->overloads()) {
    for (const Type *signature : overload.signatures) {
      const ClassDecl *cls = type->classDecl();
      FunctionDecl *fn = cls ? this->implementingMethod(cls, overload.name, signature) : nullptr;
      entries.push_back(fn ? this->lowering.classDescriptors().methodCode(fn) : null);
      entries.push_back(fn ? this->lowering.closureLowerer().functionInfo(fn) : null);
    }
  }
  auto *table = new llvm::GlobalVariable(
      *this->lowering.module_, info.table, true, llvm::GlobalValue::PrivateLinkage,
      llvm::ConstantStruct::get(info.table, entries),
      "tz.itable." + InheritLowerer::typeName(type) + "." + InheritLowerer::typeName(iface));
  table->setUnnamedAddr(llvm::GlobalValue::UnnamedAddr::Global);
  global = table;

  return global;
}

FunctionDecl *InheritLowerer::implementingMethod(const ClassDecl *cls, llvm::StringRef name,
                                                 const Type *fnType) {
  for (const ClassDecl *c = cls; c; c = c->parent) {
    std::vector<FunctionDecl *> methods;
    for (FunctionDecl *fn : c->methods) {
      if (!fn->isStatic()) {
        methods.push_back(fn);
      }
    }
    for (const ClassField &f : c->fields) {
      if (f.method()) {
        methods.push_back(f.method());
      }
    }
    for (FunctionDecl *fn : methods) {
      if (fn->name != name || fn->isAbstract() || !fn->sym) {
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

std::pair<llvm::Function *, llvm::Constant *> InheritLowerer::fieldStub(const Type *type,
                                                                        const Field &member) {
  auto &stub = this->fieldStubs[{type, member.name}];
  if (stub.first) {
    return stub;
  }
  const Type *fnType = member.type;
  auto *code = llvm::Function::Create(
      this->lowering.closureLowerer().closureFnType(fnType), llvm::GlobalValue::InternalLinkage,
      "tz." + InheritLowerer::typeName(type) + "." + member.name + ".stub",
      this->lowering.module_.get());
  this->lowering.functionBuilder().setFunctionAttributes(code);
  llvm::IRBuilderBase::InsertPoint point = this->lowering.builder.saveIP();
  this->lowering.builder.SetInsertPoint(
      llvm::BasicBlock::Create(this->lowering.ctx, "entry", code));
  llvm::Value *fn = this->lowering.builder.CreateLoad(
      this->lowering.fnTy,
      this->lowering.objectLayouts().fieldAddress(code->getArg(0), type, member.name, true),
      member.name);
  this->lowering.refCounts().retainValue(fn, fnType);
  std::vector<llvm::Value *> args;
  for (unsigned i = 1; i < code->arg_size(); ++i) {
    args.push_back(code->getArg(i));
  }
  llvm::Value *result = this->lowering.closureLowerer().callFunctionValue(fn, fnType, args);
  this->lowering.refCounts().releaseValue(fn, fnType);
  if (result) {
    this->lowering.builder.CreateRet(result);
  } else {
    this->lowering.builder.CreateRetVoid();
  }
  this->lowering.builder.restoreIP(point);
  llvm::SmallVector<llvm::UTF16, 16> units;
  llvm::convertUTF8ToUTF16String(member.name, units);
  auto *info = new llvm::GlobalVariable(
      *this->lowering.module_, this->lowering.fnInfoTy, true, llvm::GlobalValue::PrivateLinkage,
      llvm::ConstantStruct::get(this->lowering.fnInfoTy,
                                {code, this->lowering.stringLowerer().stringLiteral(
                                           std::u16string(units.begin(), units.end()))}),
      "tz.fninfo." + member.name);
  stub = {code, info};

  return stub;
}

llvm::Value *InheritLowerer::emitInterfaceCall(CallExpr *call) {
  auto *member = cast<MemberExpr>(call->callee);
  const Type *iface = member->object->type;
  const Lowering::InterfaceMember *method =
      this->dispatchedMember(iface, member->name, call->signature());
  const Type *fnType = member->type;
  if (!method) {
    for (Expr *arg : call->args) {
      this->lowering.stmtLowerer().emitDiscarded(arg);
    }
    this->lowering.builder.CreateCall(llvm::Intrinsic::getOrInsertDeclaration(
        this->lowering.module_.get(), llvm::Intrinsic::trap));
    return fnType->ret()->isVoid()
               ? nullptr
               : llvm::PoisonValue::get(this->lowering.moduleBuilder().lower(fnType->ret()));
  }
  bool borrow = ClassLowerer::staysDuringCall(member->object) &&
                llvm::all_of(call->args, AssignLowerer::cannotAssign);
  Lowering::Operand self = this->lowering.refCounts().emitOperand(member->object, borrow);
  std::vector<llvm::Value *> values = this->lowering.exprLowerer().emitExprs(call->args);
  std::vector<llvm::Value *> args{self.value};
  args.insert(args.end(), values.begin(), values.end());
  for (size_t i = call->args.size(); i < fnType->params().size(); ++i) {
    args.push_back(this->lowering.optionalLowerer().undefinedValue(fnType->params()[i]));
  }
  llvm::Value *code = InheritLowerer::invariant(this->lowering.builder.CreateLoad(
      this->lowering.builder.getPtrTy(),
      this->tableEntry(self.value, iface, unsigned(method->entry)), "code"));
  llvm::CallInst *result = this->lowering.builder.CreateCall(
      this->lowering.closureLowerer().closureFnType(fnType), code, args);
  this->lowering.refCounts().dropOperand(self);

  return result->getType()->isVoidTy() ? nullptr : result;
}

}
