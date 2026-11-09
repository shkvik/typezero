#include "compiler/lower/layouts/object_layouts/object_layouts.h"
#include "compiler/lower/classes/inherit_lowerer/inherit_lowerer.h"
#include "compiler/lower/functions/closure_lowerer/closure_lowerer.h"
#include "compiler/lower/layouts/type_descriptors/type_descriptors.h"
#include "compiler/lower/memory/group_lowerer/group_lowerer.h"
#include "compiler/lower/memory/memory_debug/memory_debug.h"
#include "compiler/lower/memory/ref_counts/ref_counts.h"
#include "compiler/lower/memory/store_lowerer/store_lowerer.h"
#include "compiler/lower/module_builder/module_builder.h"
#include "compiler/lower/runtime_functions/runtime_functions.h"
#include "compiler/lower/types/union_lowerer/union_lowerer.h"
#include "compiler/lower/types/union_member_lowerer/union_member_lowerer.h"
#include "compiler/lower/values/string_lowerer/string_lowerer.h"
#include "compiler/memory/memory_plan/memory_plan.h"
#include "llvm/ADT/STLExtras.h"
#include <numeric>

using llvm::cast;

namespace tz {

ObjectLayouts::ObjectLayouts(Lowering &owner)
    : lowering(owner) {}

std::string ObjectLayouts::layoutName(const Type *type) {
  llvm::StringRef name = type->name();

  return name.starts_with("{") ? "object" : name.str();
}

unsigned ObjectLayouts::fieldIndex(const Type *type, llvm::StringRef name) {
  llvm::ArrayRef<Field> fields = type->fields();
  for (unsigned i = 0; i < fields.size(); ++i) {
    if (fields[i].name == name) {
      return i;
    }
  }
  llvm_unreachable("Checker checked that the field exists");
}

const Lowering::ObjectLayout *ObjectLayouts::builtLayout(const Type *type) const {
  auto found = this->objectLayouts.find(type);

  return found == this->objectLayouts.end() || !found->second.type ? nullptr : &found->second;
}

const Lowering::ObjectLayout &ObjectLayouts::objectLayout(const Type *type) {
  auto [it, inserted] = this->objectLayouts.try_emplace(type);
  Lowering::ObjectLayout &layout = it->second;
  if (!inserted) {
    return layout;
  }

  llvm::ArrayRef<Field> fields = type->fields();
  const llvm::DataLayout &data = this->lowering.module_->getDataLayout();
  std::vector<unsigned> order;
  std::vector<bool> placed(fields.size(), false);
  const Type *prefix = this->lowering.inheritLowerer().layoutPrefix(type);
  if (prefix) {
    const Lowering::ObjectLayout &base = this->objectLayout(prefix);
    llvm::ArrayRef<Field> baseFields = prefix->fields();
    std::vector<unsigned> baseOrder(baseFields.size());
    std::iota(baseOrder.begin(), baseOrder.end(), 0u);
    llvm::sort(baseOrder,
               [&](unsigned a, unsigned b) { return base.elements[a] < base.elements[b]; });
    for (unsigned b : baseOrder) {
      for (unsigned i = 0; i < fields.size(); ++i) {
        if (!placed[i] && fields[i].name == baseFields[b].name &&
            (fields[i].type == baseFields[b].type ||
             this->lowering.moduleBuilder().lower(fields[i].type) ==
                 this->lowering.moduleBuilder().lower(baseFields[b].type))) {
          placed[i] = true;
          order.push_back(i);
        }
      }
    }
  }
  std::vector<unsigned> rest;
  for (unsigned i = 0; i < fields.size(); ++i) {
    if (!placed[i]) {
      rest.push_back(i);
    }
  }
  llvm::stable_sort(rest, [&](unsigned a, unsigned b) {
    bool tagA = fields[a].type->isLiteral();
    bool tagB = fields[b].type->isLiteral();
    if (tagA != tagB) {
      return tagA;
    }
    bool f = fields[a].type->nonOptional()->isFunction();
    bool g = fields[b].type->nonOptional()->isFunction();
    if (f != g) {
      return g;
    }
    uint64_t x = data.getABITypeAlign(this->lowering.moduleBuilder().lower(fields[a].type)).value();
    uint64_t y = data.getABITypeAlign(this->lowering.moduleBuilder().lower(fields[b].type)).value();

    return x != y ? x > y : fields[a].name < fields[b].name;
  });
  order.insert(order.end(), rest.begin(), rest.end());
  std::vector<llvm::Type *> elements{this->lowering.builder.getInt64Ty(),
                                     this->lowering.builder.getPtrTy()};
  layout.elements.resize(fields.size());
  for (unsigned i : order) {
    layout.elements[i] = unsigned(elements.size());
    elements.push_back(this->lowering.moduleBuilder().lower(fields[i].type));
  }
  layout.captures = unsigned(elements.size());
  const ClassDecl *cls = type->classDecl();
  if (cls && cls->group) {
    for (const VarSymbol *var : cls->group->captures) {
      elements.push_back(var->boxed()
                             ? this->lowering.builder.getPtrTy()
                             : this->lowering.moduleBuilder().lower(ClosureLowerer::typeOf(var)));
    }
  }
  layout.type = llvm::StructType::get(this->lowering.ctx, elements);

  auto *fnType = llvm::FunctionType::get(this->lowering.builder.getVoidTy(),
                                         {this->lowering.builder.getPtrTy()}, false);
  std::string name = ObjectLayouts::layoutName(type);
  llvm::StringRef runtimeDrop = type->isRegExp()                            ? "tz_re_drop"
                                : type->isWeakRef()                         ? "tz_weak_drop"
                                : type->isDateFormat()                      ? "tz_dtf_drop"
                                : type->isIntl()                            ? "tz_intl_drop"
                                : type->isCrypto()                          ? "tz_crypto_drop"
                                : type->bytes() == Type::Bytes::ArrayBuffer ? "tz_abuf_drop"
                                : type->isByteView()                        ? "tz_bytes_drop"
                                : type->isBytes()                           ? "tz_bytes_object_drop"
                                                                            : "";
  if (!runtimeDrop.empty()) {
    layout.drop = cast<llvm::Function>(
        this->lowering.module_->getOrInsertFunction(runtimeDrop, fnType).getCallee());
  } else {
    layout.drop = llvm::Function::Create(fnType, llvm::GlobalValue::InternalLinkage,
                                         "tz." + name + ".drop", this->lowering.module_.get());
  }
  layout.drop->addFnAttr(llvm::Attribute::NoUnwind);

  layout.release = llvm::Function::Create(fnType, llvm::GlobalValue::InternalLinkage,
                                          "tz." + name + ".release", this->lowering.module_.get());
  layout.release->addFnAttr(llvm::Attribute::AlwaysInline);
  layout.release->addFnAttr(llvm::Attribute::NoUnwind);
  llvm::IRBuilder<> b(llvm::BasicBlock::Create(this->lowering.ctx, "entry", layout.release));
  auto *countedBlock = llvm::BasicBlock::Create(this->lowering.ctx, "counted", layout.release);
  auto *dropBlock = llvm::BasicBlock::Create(this->lowering.ctx, "drop", layout.release);
  auto *doneBlock = llvm::BasicBlock::Create(this->lowering.ctx, "done", layout.release);
  llvm::Value *object = layout.release->getArg(0);
  b.CreateCondBr(b.CreateIsNotNull(object), countedBlock, doneBlock);
  b.SetInsertPoint(countedBlock);
  llvm::Value *objectCount = b.CreateLoad(b.getInt64Ty(), object, "rc");
  if (this->lowering.parallel_) {
    this->lowering.runtimeFunctions().countShared(b, object, objectCount, false, dropBlock,
                                                  doneBlock);
  }
  llvm::Value *left = b.CreateSub(objectCount, b.getInt64(1));
  b.CreateStore(left, object);
  b.CreateCondBr(b.CreateICmpEQ(left, b.getInt64(0)), dropBlock, doneBlock);
  b.SetInsertPoint(dropBlock);
  b.CreateCall(layout.drop, {object});
  b.CreateBr(doneBlock);
  b.SetInsertPoint(doneBlock);
  b.CreateRetVoid();

  if (runtimeDrop.empty()) {
    this->emitObjectDrop(type, layout);
  }

  return layout;
}

void ObjectLayouts::emitObjectDrop(const Type *type, Lowering::ObjectLayout &layout) {
  llvm::IRBuilderBase::InsertPoint point = this->lowering.builder.saveIP();
  llvm::Function *drop = layout.drop;
  if (this->lowering.isCyclic(type)) {
    this->lowering.storeLowerer().emitMemberDrop(type, layout);
    this->lowering.builder.restoreIP(point);
    return;
  }
  auto *entry = llvm::BasicBlock::Create(this->lowering.ctx, "entry", drop);
  auto *loopBlock = llvm::BasicBlock::Create(this->lowering.ctx, "object", drop);
  this->lowering.builder.SetInsertPoint(entry);
  this->lowering.builder.CreateBr(loopBlock);
  this->lowering.builder.SetInsertPoint(loopBlock);
  llvm::PHINode *object =
      this->lowering.builder.CreatePHI(this->lowering.builder.getPtrTy(), 2, "object");
  object->addIncoming(drop->getArg(0), entry);
  if (this->lowering.isWeakTarget(type)) {
    this->lowering.storeLowerer().forgetWeakRefs(object);
  }

  llvm::ArrayRef<Field> fields = type->fields();
  int next = -1;
  for (unsigned i = 0; i < fields.size(); ++i) {
    if (const Type *t = fields[i].type->nonOptional();
        t->isObject() && type->isSubtypeOf(t) && !this->lowering.memory().refMayCarryMember(t)) {
      next = int(i);
    }
  }
  bool exact = next >= 0 && fields[unsigned(next)].type->nonOptional() == type &&
               !this->lowering.hasSubtypes(type);
  std::vector<std::pair<llvm::Value *, const Type *>> values;
  llvm::Value *nextValue = nullptr;
  for (unsigned i = 0; i < fields.size(); ++i) {
    const Type *fieldType = fields[i].type;
    if (!fieldType->isManaged()) {
      continue;
    }
    llvm::Value *address =
        this->lowering.builder.CreateStructGEP(layout.type, object, layout.elements[i]);
    llvm::Value *value = this->lowering.builder.CreateLoad(
        this->lowering.moduleBuilder().lower(fieldType), address, fields[i].name);
    if (this->lowering.debugMemory) {
      llvm::Function *saved = this->lowering.function;
      this->lowering.function = drop;
      this->lowering.memoryDebug().checkOwned(type, fields[i].name, value, fieldType);
      this->lowering.function = saved;
    }
    if (int(i) == next) {
      nextValue = value;
    } else {
      values.push_back({value, fieldType});
    }
  }
  std::vector<llvm::Value *> boxes;
  const ClassDecl *cls = type->classDecl();
  if (cls && cls->group) {
    const std::vector<VarSymbol *> &captures = cls->group->captures;
    for (size_t i = 0; i < captures.size(); ++i) {
      const VarSymbol *var = captures[i];
      llvm::Value *address = this->lowering.builder.CreateStructGEP(layout.type, object,
                                                                    unsigned(layout.captures + i));
      if (var->boxed()) {
        boxes.push_back(this->lowering.builder.CreateLoad(this->lowering.builder.getPtrTy(),
                                                          address, var->name + ".box"));
      } else if (ClosureLowerer::typeOf(var)->isManaged()) {
        const Type *varType = ClosureLowerer::typeOf(var);
        values.push_back({this->lowering.builder.CreateLoad(
                              this->lowering.moduleBuilder().lower(varType), address, var->name),
                          varType});
      }
    }
  }
  this->lowering.builder.CreateCall(this->lowering.rt_.objFree, {object});
  for (auto [value, fieldType] : values) {
    this->lowering.refCounts().releaseValue(value, fieldType);
  }
  for (llvm::Value *box : boxes) {
    this->lowering.builder.CreateCall(this->lowering.objReleaseFn, {box});
  }
  if (nextValue) {
    auto *countedBlock = llvm::BasicBlock::Create(this->lowering.ctx, "next", drop);
    auto *doneBlock = llvm::BasicBlock::Create(this->lowering.ctx, "done", drop);
    this->lowering.builder.CreateCondBr(this->lowering.builder.CreateIsNotNull(nextValue),
                                        countedBlock, doneBlock);
    this->lowering.builder.SetInsertPoint(countedBlock);
    llvm::Value *count =
        this->lowering.builder.CreateLoad(this->lowering.builder.getInt64Ty(), nextValue, "rc");
    auto *deadBlock = llvm::BasicBlock::Create(this->lowering.ctx, "next.dead", drop);
    if (this->lowering.parallel_) {
      llvm::IRBuilder<> b(this->lowering.builder.GetInsertBlock());
      this->lowering.runtimeFunctions().countShared(b, nextValue, count, false, deadBlock,
                                                    doneBlock);
      this->lowering.builder.SetInsertPoint(b.GetInsertBlock());
    }
    llvm::Value *left = this->lowering.builder.CreateSub(count, this->lowering.builder.getInt64(1));
    this->lowering.builder.CreateStore(left, nextValue);
    this->lowering.builder.CreateCondBr(
        this->lowering.builder.CreateICmpEQ(left, this->lowering.builder.getInt64(0)), deadBlock,
        doneBlock);
    this->lowering.builder.SetInsertPoint(deadBlock);
    if (exact) {
      object->addIncoming(nextValue, deadBlock);
      this->lowering.builder.CreateBr(loopBlock);
    } else {
      auto *otherBlock = llvm::BasicBlock::Create(this->lowering.ctx, "next.other", drop);
      llvm::Value *descriptor = this->lowering.builder.CreateLoad(
          this->lowering.builder.getPtrTy(), this->lowering.builder.CreateConstInBoundsGEP1_64(
                                                 this->lowering.builder.getInt8Ty(), nextValue, 8));
      llvm::Value *nextDrop =
          this->lowering.builder.CreateLoad(this->lowering.builder.getPtrTy(), descriptor, "drop");
      object->addIncoming(nextValue, deadBlock);
      this->lowering.builder.CreateCondBr(this->lowering.builder.CreateICmpEQ(nextDrop, drop),
                                          loopBlock, otherBlock);
      this->lowering.builder.SetInsertPoint(otherBlock);
      llvm::CallInst *call = this->lowering.builder.CreateCall(
          llvm::FunctionType::get(this->lowering.builder.getVoidTy(),
                                  {this->lowering.builder.getPtrTy()}, false),
          nextDrop, {nextValue});
      call->setTailCallKind(llvm::CallInst::TCK_MustTail);
      this->lowering.builder.CreateRetVoid();
    }
    this->lowering.builder.SetInsertPoint(doneBlock);
  }
  this->lowering.builder.CreateRetVoid();
  this->lowering.builder.restoreIP(point);
}

llvm::Constant *ObjectLayouts::layoutDescriptor(const Type *type, llvm::ArrayRef<unsigned> keys) {
  return this->layoutDescriptor(type, keys,
                                type->isTuple()   ? 1
                                : type->isMatch() ? 64
                                : type->isDate()  ? 1024
                                                  : 0);
}

llvm::Constant *ObjectLayouts::layoutDescriptor(const Type *type, llvm::ArrayRef<unsigned> keys,
                                                uint32_t flags) {
  std::vector<unsigned> key(keys.begin(), keys.end());
  key.push_back(~flags);
  llvm::Constant *&descriptor = this->lowering.layoutDescriptors[{type, std::move(key)}];
  if (descriptor) {
    return descriptor;
  }
  const Lowering::ObjectLayout &layout = this->objectLayout(type);
  const llvm::StructLayout *offsets =
      this->lowering.module_->getDataLayout().getStructLayout(layout.type);
  std::vector<llvm::Constant *> entries;
  for (unsigned k : keys) {
    const Field &field = type->fields()[k];
    uint64_t offset = offsets->getElementOffset(layout.elements[k]);
    entries.push_back(llvm::ConstantStruct::get(
        this->lowering.fieldDescTy, {this->lowering.stringLowerer().utf8Constant(field.name),
                                     this->lowering.builder.getInt32(uint32_t(field.name.size())),
                                     this->lowering.builder.getInt32(uint32_t(offset)),
                                     this->lowering.typeDescriptors().typeDescriptor(field.type)}));
  }
  llvm::Constant *fields = llvm::ConstantPointerNull::get(this->lowering.builder.getPtrTy());
  if (!entries.empty()) {
    auto *arrayType = llvm::ArrayType::get(this->lowering.fieldDescTy, entries.size());
    auto *global = new llvm::GlobalVariable(*this->lowering.module_, arrayType, true,
                                            llvm::GlobalValue::PrivateLinkage,
                                            llvm::ConstantArray::get(arrayType, entries),
                                            "tz.fields." + ObjectLayouts::layoutName(type));
    global->setUnnamedAddr(llvm::GlobalValue::UnnamedAddr::Global);
    fields = global;
  }
  llvm::Constant *name = type->isWeakRef()
                             ? this->lowering.stringLowerer().utf8Constant("WeakRef")
                             : llvm::ConstantPointerNull::get(this->lowering.builder.getPtrTy());
  llvm::Constant *init = llvm::ConstantStruct::get(
      this->lowering.layoutDescTy,
      {layout.drop, fields, this->lowering.builder.getInt32(uint32_t(entries.size())),
       this->lowering.builder.getInt32(flags), name,
       this->lowering.builder.getInt32(type->isWeakRef() ? 7 : 0),
       this->lowering.builder.getInt32(
           type->isClass() ? 0 : this->lowering.unionLowerer().plainTypeId(type))});
  llvm::Constant *itables = this->lowering.inheritLowerer().itablesOf(type);
  if (!itables->isNullValue()) {
    auto *descType = llvm::StructType::get(
        this->lowering.ctx, {this->lowering.layoutDescTy, this->lowering.builder.getPtrTy()});
    init = llvm::ConstantStruct::get(descType, {init, itables});
  }
  auto *global = new llvm::GlobalVariable(*this->lowering.module_, init->getType(), true,
                                          llvm::GlobalValue::PrivateLinkage, init,
                                          "tz.layout." + ObjectLayouts::layoutName(type));
  global->setUnnamedAddr(llvm::GlobalValue::UnnamedAddr::Global);
  descriptor = global;

  return descriptor;
}

llvm::Value *ObjectLayouts::fieldAddress(llvm::Value *object, const Type *type,
                                         llvm::StringRef name, bool exact) {
  if (type->isUnion()) {
    return this->lowering.unionMemberLowerer().unionFieldAddress(object, type, name);
  }
  if (const Lowering::InterfaceMember *member =
          exact ? nullptr : this->lowering.inheritLowerer().dispatchedMember(type, name)) {
    assert(!member->callable && "a method is called or bound, not read");
    llvm::Value *offset = this->lowering.builder.CreateLoad(
        this->lowering.builder.getInt64Ty(),
        this->lowering.inheritLowerer().tableEntry(object, type, unsigned(member->entry)),
        name + ".offset");
    return this->lowering.builder.CreateInBoundsGEP(this->lowering.builder.getInt8Ty(), object,
                                                    offset, name + ".addr");
  }
  const Lowering::ObjectLayout &layout = this->objectLayout(type);

  return this->lowering.builder.CreateStructGEP(
      layout.type, object, layout.elements[ObjectLayouts::fieldIndex(type, name)], name + ".addr");
}

llvm::Value *ObjectLayouts::allocObject(const Type *type, llvm::ArrayRef<unsigned> keys) {
  return this->allocObject(type, keys, this->lowering.isCyclic(type));
}

llvm::Value *ObjectLayouts::allocObject(const Type *type, llvm::ArrayRef<unsigned> keys,
                                        bool member) {
  const Lowering::ObjectLayout &layout = this->objectLayout(type);
  llvm::Constant *descriptor = this->layoutDescriptor(type, keys);
  uint64_t size = this->lowering.module_->getDataLayout().getTypeAllocSize(layout.type);
  llvm::Value *object;
  if (member) {
    object = this->lowering.groupLowerer().allocMember(size, "object");
  } else {
    object = this->lowering.builder.CreateCall(this->lowering.rt_.objAlloc,
                                               {this->lowering.builder.getInt64(size)}, "object");
    this->lowering.builder.CreateStore(
        this->lowering.builder.getInt64(1),
        this->lowering.builder.CreateStructGEP(layout.type, object, 0));
  }
  this->lowering.builder.CreateStore(
      descriptor, this->lowering.builder.CreateStructGEP(layout.type, object, 1));

  return object;
}

}
