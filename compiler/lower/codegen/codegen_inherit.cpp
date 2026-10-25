// Inheritance and interfaces in code (docs/objects.md, sections 1 and 6).
//
// An object of a class that extends another has the fields of that class
// first, as they lie there (see objectLayout), so code that knows it by that
// class reads them at the same offsets. The descriptor of a class is
// { TzLayout, ptr itables, method table }. The method table has a slot for
// each method that a class down the chain overrides (or that is abstract),
// holding the code and the info (for the method taken as a value) of the
// method the class has there. A call through the class loads the code from
// the object's descriptor, two loads; any other call is direct.
//
// An interface is implemented by unrelated classes and by the literals of it
// and of the interfaces that extend it, whose layouts differ. A field of the
// interface that every one of them has at the offset it has in the
// interface's own layout is read there. Otherwise, and for a method that a
// class implements, code goes through a table: the descriptor of the object
// points at its tables of interfaces, by the number of the interface (no two
// interfaces that a type implements share one). A table gives the offset of
// each such field and the code and info of each such method; for an object
// that holds a function in that field, the code is a stub that calls it.

#include "compiler/lower/codegen/codegen.h"

#include "llvm/ADT/STLExtras.h"
#include "llvm/ADT/SmallPtrSet.h"
#include "llvm/Support/ConvertUTF.h"

using llvm::cast;
using llvm::dyn_cast;
using llvm::isa;

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

const Type *CodeGen::layoutPrefix(const Type *type) const {
  for (const Type *base : type->bases())
    if (!base->fields().empty())
      return base;
  return nullptr;
}

void CodeGen::prepareDispatch(SourceFile &file) {
  // The types up each declared chain; the interfaces in the order found.
  std::vector<const Type *> order;
  auto implement = [&](const Type *type, const Type *iface) {
    withSubtypes_.insert(iface);
    if (iface->isClass())
      return false;
    auto [it, inserted] = interfaces_.try_emplace(iface);
    if (inserted) {
      order.push_back(iface);
      it->second.implementers.push_back(iface);
    }
    if (llvm::is_contained(it->second.implementers, type))
      return false;
    it->second.implementers.push_back(type);
    return true;
  };
  for (const Type *type : file.subtypes) {
    if (mentionsParam(type))
      continue; // of generic code, which is emitted only as its instances
    std::vector<const Type *> queue(type->bases().begin(), type->bases().end());
    llvm::SmallPtrSet<const Type *, 8> seen;
    for (size_t i = 0; i < queue.size(); ++i) {
      const Type *base = queue[i];
      if (!seen.insert(base).second)
        continue;
      queue.insert(queue.end(), base->bases().begin(), base->bases().end());
      implement(type, base);
    }
  }
  // A type with read-only fields that objects of another are seen as is
  // one too, of what the other's objects may be.
  for (bool changed = true; changed;) {
    changed = false;
    for (auto [from, to] : file.views) {
      if (mentionsParam(from) || mentionsParam(to))
        continue;
      changed |= implement(from, to);
      if (auto found = interfaces_.find(from); found != interfaces_.end()) {
        std::vector<const Type *> others = found->second.implementers;
        for (const Type *other : others)
          changed |= implement(other, to);
      }
    }
  }

  const llvm::DataLayout &data = module_->getDataLayout();
  auto offsetOf = [&](const Type *type, unsigned field) {
    const ObjectLayout &layout = objectLayout(type);
    return data.getStructLayout(layout.type)->getElementOffset(layout.elements[field]);
  };
  for (const Type *iface : order) {
    InterfaceInfo &info = interfaces_[iface];
    std::vector<llvm::Type *> entries;
    llvm::ArrayRef<Field> fields = iface->fields();
    info.members.resize(fields.size());
    for (unsigned k = 0; k < fields.size(); ++k) {
      const Field &f = fields[k];
      InterfaceMember &member = info.members[k];
      // A method of some class that implements it.
      bool callable = f.type->isFunction() && llvm::any_of(info.implementers, [&](const Type *t) {
                        return t->isClass() && !t->field(f.name);
                      });
      if (callable) {
        member.entry = int(entries.size());
        member.callable = true;
        entries.push_back(builder_.getPtrTy()); // the code
        entries.push_back(builder_.getPtrTy()); // its info
        continue;
      }
      uint64_t offset = offsetOf(iface, k);
      if (llvm::all_of(info.implementers,
                       [&](const Type *t) { return offsetOf(t, fieldIndex(t, f.name)) == offset; }))
        continue;
      member.entry = int(entries.size());
      entries.push_back(builder_.getInt64Ty()); // the offset
    }
    // A method with several signatures is a method of classes only.
    for (const Overload &overload : iface->overloads()) {
      std::vector<InterfaceMember> &signatures = info.overloads.emplace_back();
      for (size_t i = 0; i < overload.signatures.size(); ++i) {
        signatures.push_back({int(entries.size()), true});
        entries.push_back(builder_.getPtrTy());
        entries.push_back(builder_.getPtrTy());
      }
    }
    info.dispatch = !entries.empty();
    info.table = llvm::StructType::get(ctx_, entries);
  }

  // The numbers: interfaces that some type implements together differ.
  std::vector<const Type *> numbered;
  for (const Type *iface : order) {
    InterfaceInfo &info = interfaces_[iface];
    if (!info.dispatch)
      continue;
    llvm::SmallVector<unsigned, 4> taken;
    for (const Type *other : numbered) {
      const InterfaceInfo &o = interfaces_[other];
      if (llvm::any_of(info.implementers,
                       [&](const Type *t) { return llvm::is_contained(o.implementers, t); }))
        taken.push_back(o.color);
    }
    while (llvm::is_contained(taken, info.color))
      ++info.color;
    colors_ = std::max(colors_, info.color + 1);
    numbered.push_back(iface);
    for (const Type *t : info.implementers)
      tablesOf_[t].push_back(iface);
  }
}

const CodeGen::InterfaceMember *CodeGen::dispatchedMember(const Type *type, llvm::StringRef name,
                                                          int signature) {
  if (!type->isObject())
    return nullptr;
  auto found = interfaces_.find(type);
  if (found == interfaces_.end() || !found->second.dispatch)
    return nullptr;
  if (signature >= 0) {
    llvm::ArrayRef<Overload> overloads = type->overloads();
    for (size_t i = 0; i < overloads.size(); ++i)
      if (overloads[i].name == name)
        return &found->second.overloads[i][size_t(signature)];
    return nullptr;
  }
  if (!type->field(name))
    return nullptr;
  const InterfaceMember &member = found->second.members[fieldIndex(type, name)];
  return member.entry >= 0 ? &member : nullptr;
}

llvm::Value *CodeGen::tableEntry(llvm::Value *object, const Type *iface, unsigned entry) {
  const InterfaceInfo &info = interfaces_.find(iface)->second;
  llvm::Type *ptr = builder_.getPtrTy();
  llvm::Value *descriptor = builder_.CreateLoad(
      ptr, builder_.CreateConstInBoundsGEP1_64(builder_.getInt8Ty(), object, 8), "descriptor");
  llvm::Value *itables = invariant(builder_.CreateLoad(
      ptr, builder_.CreateConstInBoundsGEP1_64(builder_.getInt8Ty(), descriptor, kItablesOffset),
      "itables"));
  llvm::Value *table = invariant(builder_.CreateLoad(
      ptr, builder_.CreateConstInBoundsGEP1_64(ptr, itables, info.color), "itable"));
  return builder_.CreateStructGEP(info.table, table, entry);
}

llvm::Value *CodeGen::vtableEntry(llvm::Value *object, int slot, bool info) {
  llvm::Type *ptr = builder_.getPtrTy();
  llvm::Value *descriptor = builder_.CreateLoad(
      ptr, builder_.CreateConstInBoundsGEP1_64(builder_.getInt8Ty(), object, 8), "descriptor");
  uint64_t offset = kVtableOffset + 16 * uint64_t(slot) + (info ? 8 : 0);
  return invariant(builder_.CreateLoad(
      ptr, builder_.CreateConstInBoundsGEP1_64(builder_.getInt8Ty(), descriptor, offset),
      info ? "info" : "code"));
}

llvm::Constant *CodeGen::itablesOf(const Type *type) {
  llvm::Constant *null = llvm::ConstantPointerNull::get(builder_.getPtrTy());
  auto found = tablesOf_.find(type);
  if (found == tablesOf_.end())
    return null;
  llvm::Constant *&global = itables_[type];
  if (global)
    return global;
  std::vector<llvm::Constant *> tables(colors_, null);
  for (const Type *iface : found->second)
    tables[interfaces_[iface].color] = itable(type, iface);
  auto *arrayType = llvm::ArrayType::get(builder_.getPtrTy(), colors_);
  auto *array = new llvm::GlobalVariable(
      *module_, arrayType, /*isConstant=*/true, llvm::GlobalValue::PrivateLinkage,
      llvm::ConstantArray::get(arrayType, tables), "tz.itables." + typeName(type));
  array->setUnnamedAddr(llvm::GlobalValue::UnnamedAddr::Global);
  global = array;
  return global;
}

llvm::Constant *CodeGen::itable(const Type *type, const Type *iface) {
  llvm::Constant *&global = itableConstants_[{type, iface}];
  if (global)
    return global;
  const InterfaceInfo &info = interfaces_[iface];
  const llvm::DataLayout &data = module_->getDataLayout();
  llvm::Constant *null = llvm::ConstantPointerNull::get(builder_.getPtrTy());
  std::vector<llvm::Constant *> entries;
  llvm::ArrayRef<Field> fields = iface->fields();
  for (unsigned k = 0; k < fields.size(); ++k) {
    const InterfaceMember &member = info.members[k];
    if (member.entry < 0)
      continue;
    const Field &f = fields[k];
    if (!member.callable) {
      const ObjectLayout &layout = objectLayout(type);
      uint64_t offset = data.getStructLayout(layout.type)
                            ->getElementOffset(layout.elements[fieldIndex(type, f.name)]);
      entries.push_back(builder_.getInt64(offset));
      continue;
    }
    if (const ClassDecl *cls = type->classDecl(); cls && !type->field(f.name)) {
      // A class with the method: an abstract class has no objects.
      FunctionDecl *fn = implementingMethod(cls, f.name, f.type);
      entries.push_back(fn ? methodCode(fn) : null);
      entries.push_back(fn ? functionInfo(fn) : null);
      continue;
    }
    auto [code, codeInfo] = fieldStub(type, f);
    entries.push_back(code);
    entries.push_back(codeInfo);
  }
  // The variant of a class for each signature; a type that is not a class
  // has no objects with such methods.
  for (const Overload &overload : iface->overloads())
    for (const Type *signature : overload.signatures) {
      const ClassDecl *cls = type->classDecl();
      FunctionDecl *fn = cls ? implementingMethod(cls, overload.name, signature) : nullptr;
      entries.push_back(fn ? methodCode(fn) : null);
      entries.push_back(fn ? functionInfo(fn) : null);
    }
  auto *table = new llvm::GlobalVariable(*module_, info.table, /*isConstant=*/true,
                                         llvm::GlobalValue::PrivateLinkage,
                                         llvm::ConstantStruct::get(info.table, entries),
                                         "tz.itable." + typeName(type) + "." + typeName(iface));
  table->setUnnamedAddr(llvm::GlobalValue::UnnamedAddr::Global);
  global = table;
  return global;
}

FunctionDecl *CodeGen::implementingMethod(const ClassDecl *cls, llvm::StringRef name,
                                          const Type *fnType) {
  for (const ClassDecl *c = cls; c; c = c->parent) {
    std::vector<FunctionDecl *> methods;
    for (FunctionDecl *fn : c->methods)
      if (!fn->isStatic)
        methods.push_back(fn);
    for (const ClassField &f : c->fields)
      if (f.method)
        methods.push_back(f.method);
    for (FunctionDecl *fn : methods) {
      if (fn->name != name || fn->isAbstract || !fn->sym)
        continue;
      const Type *type = functionTypeOf(fn->sym);
      if (type->params() == fnType->params() && type->minArgs() == fnType->minArgs() &&
          type->hasRest() == fnType->hasRest())
        return fn;
    }
  }
  return nullptr;
}

// The function in the field is kept alive while it runs: the call may replace
// what the field holds.
std::pair<llvm::Function *, llvm::Constant *> CodeGen::fieldStub(const Type *type,
                                                                 const Field &member) {
  auto &stub = fieldStubs_[{type, member.name}];
  if (stub.first)
    return stub;
  const Type *fnType = member.type;
  auto *code =
      llvm::Function::Create(closureFnType(fnType), llvm::GlobalValue::InternalLinkage,
                             "tz." + typeName(type) + "." + member.name + ".stub", module_.get());
  setFunctionAttributes(code);
  llvm::IRBuilderBase::InsertPoint point = builder_.saveIP();
  builder_.SetInsertPoint(llvm::BasicBlock::Create(ctx_, "entry", code));
  llvm::Value *fn = builder_.CreateLoad(
      fnTy_, fieldAddress(code->getArg(0), type, member.name, /*exact=*/true), member.name);
  retainValue(fn, fnType);
  std::vector<llvm::Value *> args;
  for (unsigned i = 1; i < code->arg_size(); ++i)
    args.push_back(code->getArg(i));
  llvm::Value *result = callFunctionValue(fn, fnType, args);
  releaseValue(fn, fnType);
  if (result)
    builder_.CreateRet(result);
  else
    builder_.CreateRetVoid();
  builder_.restoreIP(point);
  llvm::SmallVector<llvm::UTF16, 16> units;
  llvm::convertUTF8ToUTF16String(member.name, units);
  auto *info = new llvm::GlobalVariable(
      *module_, fnInfoTy_, /*isConstant=*/true, llvm::GlobalValue::PrivateLinkage,
      llvm::ConstantStruct::get(fnInfoTy_,
                                {code, stringLiteral(std::u16string(units.begin(), units.end()))}),
      "tz.fninfo." + member.name);
  stub = {code, info};
  return stub;
}

// `shape.area()` through an interface: the code the object's table gives,
// called with the object, as a method is.
llvm::Value *CodeGen::emitInterfaceCall(CallExpr *call) {
  auto *member = cast<MemberExpr>(call->callee);
  const Type *iface = member->object->type;
  const InterfaceMember *method = dispatchedMember(iface, member->name, call->signature);
  const Type *fnType = member->type;
  if (!method) {
    // A method with several signatures of an interface that no class
    // implements: no object can get here.
    for (Expr *arg : call->args)
      emitDiscarded(arg);
    builder_.CreateCall(
        llvm::Intrinsic::getOrInsertDeclaration(module_.get(), llvm::Intrinsic::trap));
    return fnType->ret()->isVoid() ? nullptr : llvm::PoisonValue::get(lower(fnType->ret()));
  }
  bool borrow = staysDuringCall(member->object) && llvm::all_of(call->args, cannotAssign);
  Operand self = emitOperand(member->object, borrow);
  // The object waits while an argument that may throw is computed.
  holdOperand(self, llvm::any_of(call->args, [&](Expr *arg) { return exprThrows(arg); }));
  std::vector<llvm::Value *> values = emitHeldValues(call->args);
  takeOperand(self);
  std::vector<llvm::Value *> args{self.value};
  args.insert(args.end(), values.begin(), values.end());
  for (size_t i = call->args.size(); i < fnType->params().size(); ++i)
    args.push_back(undefinedValue(fnType->params()[i]));
  llvm::Value *code = invariant(builder_.CreateLoad(
      builder_.getPtrTy(), tableEntry(self.value, iface, unsigned(method->entry)), "code"));
  llvm::CallInst *result = builder_.CreateCall(closureFnType(fnType), code, args);
  // The object goes before what the method threw does.
  dropOperand(self);
  emitThrowCheckIf(typeThrows(fnType));
  return result->getType()->isVoidTy() ? nullptr : result;
}

} // namespace tz
