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
#include "llvm/Support/ConvertUTF.h"
#include <numeric>

using llvm::BasicBlock;
using llvm::cast;
using llvm::dyn_cast;
using llvm::isa;
using llvm::Value;

namespace tz {

namespace {

// A name for the functions of a layout in the IR.
std::string layoutName(const Type *type) {
  llvm::StringRef name = type->name();

  return name.starts_with("{") ? "object" : name.str();
}

} // namespace

ObjectLayouts::ObjectLayouts(Lowering &lowering) : lowering_(lowering) {}

unsigned ObjectLayouts::fieldIndex(const Type *type, llvm::StringRef name) {
  llvm::ArrayRef<Field> fields = type->fields();
  for (unsigned i = 0; i < fields.size(); ++i) {
    if (fields[i].name == name) {
      return i;
    }
  }
  llvm_unreachable("Checker checked that the field exists");
}

const Lowering::ObjectLayout &ObjectLayouts::objectLayout(const Type *type) {
  auto [it, inserted] = objectLayouts_.try_emplace(type);
  Lowering::ObjectLayout &layout = it->second;
  if (!inserted) {
    return layout;
  }

  // The fields of the class it extends come first, as they lie there, so
  // that code that knows the object by that class reads them at the same
  // offsets; the same, if it can, for an interface it implements.
  llvm::ArrayRef<Field> fields = type->fields();
  const llvm::DataLayout &data = lowering_.module_->getDataLayout();
  std::vector<unsigned> order;
  std::vector<bool> placed(fields.size(), false);
  if (const Type *prefix = lowering_.inheritLowerer().layoutPrefix(type)) {
    const Lowering::ObjectLayout &base = objectLayout(prefix);
    llvm::ArrayRef<Field> baseFields = prefix->fields();
    std::vector<unsigned> baseOrder(baseFields.size());
    std::iota(baseOrder.begin(), baseOrder.end(), 0u);
    llvm::sort(baseOrder,
               [&](unsigned a, unsigned b) { return base.elements[a] < base.elements[b]; });
    // A field the type narrows (`pet: Dog` for `pet: Animal`, see
    // sema_widen.cpp) lies as a reference too.
    for (unsigned b : baseOrder) {
      for (unsigned i = 0; i < fields.size(); ++i) {
        if (!placed[i] && fields[i].name == baseFields[b].name &&
            (fields[i].type == baseFields[b].type ||
             lowering_.moduleBuilder().lower(fields[i].type) ==
                 lowering_.moduleBuilder().lower(baseFields[b].type))) {
          placed[i] = true;
          order.push_back(i);
        }
      }
    }
  }
  // The others by alignment, largest first, so that each field starts where
  // the one before it ends; then by name, so that the order does not depend
  // on the declaration. Functions go last: an interface's methods are fields
  // that hold functions, which a class implementing it does not have, and so
  // the other fields of the interface lie where the class puts them. A field
  // of a literal type goes first: it is the tag that tells the members of a
  // union apart (`kind: "circle"`), which then lies at one offset in each of
  // them, and `s.kind` is one load.
  std::vector<unsigned> rest;
  for (unsigned i = 0; i < fields.size(); ++i) {
    if (!placed[i]) {
      rest.push_back(i);
    }
  }
  llvm::stable_sort(rest, [&](unsigned a, unsigned b) {
    bool tagA = fields[a].type->isLiteral(), tagB = fields[b].type->isLiteral();
    if (tagA != tagB) {
      return tagA;
    }
    bool f = fields[a].type->nonOptional()->isFunction();
    bool g = fields[b].type->nonOptional()->isFunction();
    if (f != g) {
      return g;
    }
    uint64_t x = data.getABITypeAlign(lowering_.moduleBuilder().lower(fields[a].type)).value();
    uint64_t y = data.getABITypeAlign(lowering_.moduleBuilder().lower(fields[b].type)).value();

    return x != y ? x > y : fields[a].name < fields[b].name;
  });
  order.insert(order.end(), rest.begin(), rest.end());
  std::vector<llvm::Type *> elements{lowering_.builder_.getInt64Ty(),
                                     lowering_.builder_.getPtrTy()};
  layout.elements.resize(fields.size());
  for (unsigned i : order) {
    layout.elements[i] = unsigned(elements.size());
    elements.push_back(lowering_.moduleBuilder().lower(fields[i].type));
  }
  layout.captures = unsigned(elements.size());
  if (const ClassDecl *cls = type->classDecl(); cls && cls->group) {
    for (const VarSymbol *var : cls->group->captures) {
      elements.push_back(var->boxed()
                             ? lowering_.builder_.getPtrTy()
                             : lowering_.moduleBuilder().lower(ClosureLowerer::typeOf(var)));
    }
  }
  layout.type = llvm::StructType::get(lowering_.ctx_, elements);

  auto *fnType = llvm::FunctionType::get(lowering_.builder_.getVoidTy(),
                                         {lowering_.builder_.getPtrTy()}, false);
  std::string name = layoutName(type);
  // The runtime makes regexes, and frees them (see TzRegex in
  // runtime/library/regex/regex/regex.h); so with formatters of dates, the other objects
  // of Intl (which the object's own layout frees, tz_intl_drop) and the objects
  // of binary data.
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
        lowering_.module_->getOrInsertFunction(runtimeDrop, fnType).getCallee());
  } else {
    layout.drop = llvm::Function::Create(fnType, llvm::GlobalValue::InternalLinkage,
                                         "tz." + name + ".drop", lowering_.module_.get());
  }
  layout.drop->addFnAttr(llvm::Attribute::NoUnwind);

  // The release: the count is the first word; the last reference drops the
  // object. Undefined (null) holds nothing.
  layout.release = llvm::Function::Create(fnType, llvm::GlobalValue::InternalLinkage,
                                          "tz." + name + ".release", lowering_.module_.get());
  layout.release->addFnAttr(llvm::Attribute::AlwaysInline);
  layout.release->addFnAttr(llvm::Attribute::NoUnwind);
  llvm::IRBuilder<> b(llvm::BasicBlock::Create(lowering_.ctx_, "entry", layout.release));
  auto *countedBlock = llvm::BasicBlock::Create(lowering_.ctx_, "counted", layout.release);
  auto *dropBlock = llvm::BasicBlock::Create(lowering_.ctx_, "drop", layout.release);
  auto *doneBlock = llvm::BasicBlock::Create(lowering_.ctx_, "done", layout.release);
  llvm::Value *object = layout.release->getArg(0);
  b.CreateCondBr(b.CreateIsNotNull(object), countedBlock, doneBlock);
  b.SetInsertPoint(countedBlock);
  llvm::Value *objectCount = b.CreateLoad(b.getInt64Ty(), object, "rc");
  if (lowering_.parallel_) {
    lowering_.runtimeFunctions().countShared(b, object, objectCount, /*retain=*/false, dropBlock,
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
    emitObjectDrop(type, layout);
  }

  return layout;
}

// Releases what the fields hold, then frees the object. A list (`next?:
// Node`) would make that recursion as deep as the list is long, so a field
// that holds an object of the same type is released last, by going round
// again: freeing a long list takes no stack. If the field may hold objects of
// other types too (a class up the chain, or one with subclasses), it goes
// round again for the next object whose drop function is this one.
void ObjectLayouts::emitObjectDrop(const Type *type, Lowering::ObjectLayout &layout) {
  llvm::IRBuilderBase::InsertPoint point = lowering_.builder_.saveIP();
  llvm::Function *drop = layout.drop;
  if (lowering_.isCyclic(type)) {
    lowering_.storeLowerer().emitMemberDrop(type, layout);
    lowering_.builder_.restoreIP(point);
    return;
  }
  auto *entry = llvm::BasicBlock::Create(lowering_.ctx_, "entry", drop);
  auto *loopBlock = llvm::BasicBlock::Create(lowering_.ctx_, "object", drop);
  lowering_.builder_.SetInsertPoint(entry);
  lowering_.builder_.CreateBr(loopBlock);
  lowering_.builder_.SetInsertPoint(loopBlock);
  llvm::PHINode *object = lowering_.builder_.CreatePHI(lowering_.builder_.getPtrTy(), 2, "object");
  object->addIncoming(drop->getArg(0), entry);
  if (lowering_.isWeakTarget(type)) {
    lowering_.storeLowerer().forgetWeakRefs(object);
  }

  llvm::ArrayRef<Field> fields = type->fields();
  int next = -1; // the field of the same type, released last
  for (unsigned i = 0; i < fields.size(); ++i) {
    if (const Type *t = fields[i].type->nonOptional();
        t->isObject() && type->isSubtypeOf(t) && !lowering_.memory().refMayBeMember(t)) {
      next = int(i);
    }
  }
  bool exact = next >= 0 && fields[unsigned(next)].type->nonOptional() == type &&
               !lowering_.hasSubtypes(type);
  std::vector<std::pair<llvm::Value *, const Type *>> values;
  llvm::Value *nextValue = nullptr;
  for (unsigned i = 0; i < fields.size(); ++i) {
    const Type *fieldType = fields[i].type;
    if (!fieldType->isManaged()) {
      continue;
    }
    llvm::Value *address =
        lowering_.builder_.CreateStructGEP(layout.type, object, layout.elements[i]);
    llvm::Value *value = lowering_.builder_.CreateLoad(lowering_.moduleBuilder().lower(fieldType),
                                                       address, fields[i].name);
    if (lowering_.debugMemory_) {
      llvm::Function *saved = lowering_.function_;
      lowering_.function_ = drop;
      lowering_.memoryDebug().checkOwned(type, fields[i].name, value, fieldType);
      lowering_.function_ = saved;
    }
    if (int(i) == next) {
      nextValue = value;
    } else {
      values.push_back({value, fieldType});
    }
  }
  // The variables a literal's methods captured.
  std::vector<llvm::Value *> boxes;
  if (const ClassDecl *cls = type->classDecl(); cls && cls->group) {
    const std::vector<VarSymbol *> &captures = cls->group->captures;
    for (size_t i = 0; i < captures.size(); ++i) {
      const VarSymbol *var = captures[i];
      llvm::Value *address =
          lowering_.builder_.CreateStructGEP(layout.type, object, unsigned(layout.captures + i));
      if (var->boxed()) {
        boxes.push_back(lowering_.builder_.CreateLoad(lowering_.builder_.getPtrTy(), address,
                                                      var->name + ".box"));
      } else if (ClosureLowerer::typeOf(var)->isManaged()) {
        const Type *varType = ClosureLowerer::typeOf(var);
        values.push_back({lowering_.builder_.CreateLoad(lowering_.moduleBuilder().lower(varType),
                                                        address, var->name),
                          varType});
      }
    }
  }
  lowering_.builder_.CreateCall(lowering_.rt_.objFree, {object});
  for (auto [value, fieldType] : values) {
    lowering_.refCounts().releaseValue(value, fieldType);
  }
  for (llvm::Value *box : boxes) {
    lowering_.builder_.CreateCall(lowering_.objReleaseFn_, {box});
  }
  if (nextValue) {
    auto *countedBlock = llvm::BasicBlock::Create(lowering_.ctx_, "next", drop);
    auto *doneBlock = llvm::BasicBlock::Create(lowering_.ctx_, "done", drop);
    lowering_.builder_.CreateCondBr(lowering_.builder_.CreateIsNotNull(nextValue), countedBlock,
                                    doneBlock);
    lowering_.builder_.SetInsertPoint(countedBlock);
    llvm::Value *count =
        lowering_.builder_.CreateLoad(lowering_.builder_.getInt64Ty(), nextValue, "rc");
    // The next object dies with this one when this held its last reference:
    // a shared one's count goes atomically (runtime/core/publish/publish.c).
    auto *deadBlock = llvm::BasicBlock::Create(lowering_.ctx_, "next.dead", drop);
    if (lowering_.parallel_) {
      llvm::IRBuilder<> b(lowering_.builder_.GetInsertBlock());
      lowering_.runtimeFunctions().countShared(b, nextValue, count, /*retain=*/false, deadBlock,
                                               doneBlock);
      lowering_.builder_.SetInsertPoint(b.GetInsertBlock());
    }
    llvm::Value *left = lowering_.builder_.CreateSub(count, lowering_.builder_.getInt64(1));
    lowering_.builder_.CreateStore(left, nextValue);
    lowering_.builder_.CreateCondBr(
        lowering_.builder_.CreateICmpEQ(left, lowering_.builder_.getInt64(0)), deadBlock,
        doneBlock);
    lowering_.builder_.SetInsertPoint(deadBlock);
    if (exact) {
      object->addIncoming(nextValue, deadBlock);
      lowering_.builder_.CreateBr(loopBlock);
    } else {
      // The drop function of the next object's own descriptor.
      auto *otherBlock = llvm::BasicBlock::Create(lowering_.ctx_, "next.other", drop);
      llvm::Value *descriptor = lowering_.builder_.CreateLoad(
          lowering_.builder_.getPtrTy(), lowering_.builder_.CreateConstInBoundsGEP1_64(
                                             lowering_.builder_.getInt8Ty(), nextValue, 8));
      llvm::Value *nextDrop =
          lowering_.builder_.CreateLoad(lowering_.builder_.getPtrTy(), descriptor, "drop");
      object->addIncoming(nextValue, deadBlock);
      lowering_.builder_.CreateCondBr(lowering_.builder_.CreateICmpEQ(nextDrop, drop), loopBlock,
                                      otherBlock);
      // Another type's drop, as a tail call even without optimizations: a
      // list of objects of mixed types takes no stack either.
      lowering_.builder_.SetInsertPoint(otherBlock);
      llvm::CallInst *call = lowering_.builder_.CreateCall(
          llvm::FunctionType::get(lowering_.builder_.getVoidTy(), {lowering_.builder_.getPtrTy()},
                                  false),
          nextDrop, {nextValue});
      call->setTailCallKind(llvm::CallInst::TCK_MustTail);
      lowering_.builder_.CreateRetVoid();
    }
    lowering_.builder_.SetInsertPoint(doneBlock);
  }
  lowering_.builder_.CreateRetVoid();
  lowering_.builder_.restoreIP(point);
}

llvm::Constant *ObjectLayouts::layoutDescriptor(const Type *type, llvm::ArrayRef<unsigned> keys) {
  // TZ_LAYOUT_TUPLE, TZ_LAYOUT_MATCH and TZ_LAYOUT_DATE
  // (runtime/core/tzrt/tzrt.h).
  return layoutDescriptor(type, keys,
                          type->isTuple()   ? 1
                          : type->isMatch() ? 64
                          : type->isDate()  ? 1024
                                            : 0);
}

llvm::Constant *ObjectLayouts::layoutDescriptor(const Type *type, llvm::ArrayRef<unsigned> keys,
                                                uint32_t flags) {
  std::vector<unsigned> key(keys.begin(), keys.end());
  key.push_back(~flags); // apart from the keys, which are field positions
  llvm::Constant *&descriptor = lowering_.layoutDescriptors_[{type, std::move(key)}];
  if (descriptor) {
    return descriptor;
  }
  const Lowering::ObjectLayout &layout = objectLayout(type);
  const llvm::StructLayout *offsets =
      lowering_.module_->getDataLayout().getStructLayout(layout.type);
  std::vector<llvm::Constant *> entries;
  for (unsigned k : keys) {
    const Field &field = type->fields()[k];
    uint64_t offset = offsets->getElementOffset(layout.elements[k]);
    entries.push_back(llvm::ConstantStruct::get(
        lowering_.fieldDescTy_, {lowering_.stringLowerer().utf8Constant(field.name),
                                 lowering_.builder_.getInt32(uint32_t(field.name.size())),
                                 lowering_.builder_.getInt32(uint32_t(offset)),
                                 lowering_.typeDescriptors().typeDescriptor(field.type)}));
  }
  llvm::Constant *fields = llvm::ConstantPointerNull::get(lowering_.builder_.getPtrTy());
  if (!entries.empty()) {
    auto *arrayType = llvm::ArrayType::get(lowering_.fieldDescTy_, entries.size());
    auto *global = new llvm::GlobalVariable(
        *lowering_.module_, arrayType, /*isConstant=*/true, llvm::GlobalValue::PrivateLinkage,
        llvm::ConstantArray::get(arrayType, entries), "tz.fields." + layoutName(type));
    global->setUnnamedAddr(llvm::GlobalValue::UnnamedAddr::Global);
    fields = global;
  }
  // The number that tells objects of this type apart from those of the
  // other members of a union (see codegen_union.cpp). A WeakRef prints as
  // node prints one, `WeakRef {}`.
  llvm::Constant *name = type->isWeakRef()
                             ? lowering_.stringLowerer().utf8Constant("WeakRef")
                             : llvm::ConstantPointerNull::get(lowering_.builder_.getPtrTy());
  llvm::Constant *init = llvm::ConstantStruct::get(
      lowering_.layoutDescTy_,
      {layout.drop, fields, lowering_.builder_.getInt32(uint32_t(entries.size())),
       lowering_.builder_.getInt32(flags), name,
       lowering_.builder_.getInt32(type->isWeakRef() ? 7 : 0),
       lowering_.builder_.getInt32(type->isClass() ? 0
                                                   : lowering_.unionLowerer().plainTypeId(type))});
  // An object of a type that implements interfaces with tables: then the
  // tables (see codegen_inherit.cpp).
  llvm::Constant *itables = lowering_.inheritLowerer().itablesOf(type);
  if (!itables->isNullValue()) {
    auto *descType = llvm::StructType::get(
        lowering_.ctx_, {lowering_.layoutDescTy_, lowering_.builder_.getPtrTy()});
    init = llvm::ConstantStruct::get(descType, {init, itables});
  }
  auto *global = new llvm::GlobalVariable(*lowering_.module_, init->getType(), /*isConstant=*/true,
                                          llvm::GlobalValue::PrivateLinkage, init,
                                          "tz.layout." + layoutName(type));
  global->setUnnamedAddr(llvm::GlobalValue::UnnamedAddr::Global);
  descriptor = global;

  return descriptor;
}

llvm::Value *ObjectLayouts::fieldAddress(llvm::Value *object, const Type *type,
                                         llvm::StringRef name, bool exact) {
  if (type->isUnion()) { // in each member, where it is there (codegen_union.cpp)
    return lowering_.unionMemberLowerer().unionFieldAddress(object, type, name);
  }
  // Through an interface whose implementers put the field elsewhere: at the
  // offset the object's table gives.
  if (const Lowering::InterfaceMember *member =
          exact ? nullptr : lowering_.inheritLowerer().dispatchedMember(type, name)) {
    assert(!member->callable && "a method is called or bound, not read");
    llvm::Value *offset = lowering_.builder_.CreateLoad(
        lowering_.builder_.getInt64Ty(),
        lowering_.inheritLowerer().tableEntry(object, type, unsigned(member->entry)),
        name + ".offset");
    return lowering_.builder_.CreateInBoundsGEP(lowering_.builder_.getInt8Ty(), object, offset,
                                                name + ".addr");
  }
  const Lowering::ObjectLayout &layout = objectLayout(type);

  return lowering_.builder_.CreateStructGEP(
      layout.type, object, layout.elements[fieldIndex(type, name)], name + ".addr");
}

llvm::Value *ObjectLayouts::allocObject(const Type *type, llvm::ArrayRef<unsigned> keys) {
  return allocObject(type, keys, lowering_.isCyclic(type));
}

llvm::Value *ObjectLayouts::allocObject(const Type *type, llvm::ArrayRef<unsigned> keys,
                                        bool member) {
  const Lowering::ObjectLayout &layout = objectLayout(type);
  llvm::Constant *descriptor = layoutDescriptor(type, keys);
  uint64_t size = lowering_.module_->getDataLayout().getTypeAllocSize(layout.type);
  llvm::Value *object;
  if (member) {
    object = lowering_.groupLowerer().allocMember(size, "object"); // counted by its group
  } else {
    object = lowering_.builder_.CreateCall(lowering_.rt_.objAlloc,
                                           {lowering_.builder_.getInt64(size)}, "object");
    lowering_.builder_.CreateStore(lowering_.builder_.getInt64(1),
                                   lowering_.builder_.CreateStructGEP(layout.type, object, 0));
  }
  lowering_.builder_.CreateStore(descriptor,
                                 lowering_.builder_.CreateStructGEP(layout.type, object, 1));

  return object;
}

} // namespace tz
