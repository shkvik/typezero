// Objects: their layouts, the descriptors the runtime gets, literals, and
// reading and writing fields.
//
// An object is a reference to { i64 rc, ptr layout, fields... }, allocated by
// tz_obj_alloc (runtime/closure.c) as the environments of closures are. The
// fields are stored unboxed, each in the representation of its type (an
// optional field as `T | undefined`), at offsets fixed by the type, so
// reading one is a single load.
//
// `layout` points at a constant descriptor (TzLayout in runtime/tzrt.h): the
// function that drops the object, and for printing and Object.keys the
// names, offsets and types of the fields in the order of the literal that
// created the object. Node prints the keys of an object in the order they
// were added, so one type may have several descriptors, which differ only in
// that order. A tuple is an object with the fields "0", "1"... whose
// descriptor says that it prints as an array.

#include "codegen/codegen.h"

#include "llvm/ADT/STLExtras.h"
#include "llvm/Support/ConvertUTF.h"

#include <numeric>

using llvm::cast;
using llvm::dyn_cast;
using llvm::isa;

namespace tz {

unsigned CodeGen::fieldIndex(const Type *type, llvm::StringRef name) {
  llvm::ArrayRef<Field> fields = type->fields();
  for (unsigned i = 0; i < fields.size(); ++i)
    if (fields[i].name == name)
      return i;
  llvm_unreachable("Sema checked that the field exists");
}

namespace {

// A name for the functions of a layout in the IR.
std::string layoutName(const Type *type) {
  llvm::StringRef name = type->name();
  return name.starts_with("{") ? "object" : name.str();
}

} // namespace

const CodeGen::ObjectLayout &CodeGen::objectLayout(const Type *type) {
  auto [it, inserted] = objectLayouts_.try_emplace(type);
  ObjectLayout &layout = it->second;
  if (!inserted)
    return layout;

  // The fields of the class it extends come first, as they lie there, so
  // that code that knows the object by that class reads them at the same
  // offsets; the same, if it can, for an interface it implements.
  llvm::ArrayRef<Field> fields = type->fields();
  const llvm::DataLayout &data = module_->getDataLayout();
  std::vector<unsigned> order;
  std::vector<bool> placed(fields.size(), false);
  if (const Type *prefix = layoutPrefix(type)) {
    const ObjectLayout &base = objectLayout(prefix);
    llvm::ArrayRef<Field> baseFields = prefix->fields();
    std::vector<unsigned> baseOrder(baseFields.size());
    std::iota(baseOrder.begin(), baseOrder.end(), 0u);
    llvm::sort(baseOrder,
               [&](unsigned a, unsigned b) { return base.elements[a] < base.elements[b]; });
    // A field the type narrows (`pet: Dog` for `pet: Animal`, see
    // sema_widen.cpp) lies as a reference too.
    for (unsigned b : baseOrder)
      for (unsigned i = 0; i < fields.size(); ++i)
        if (!placed[i] && fields[i].name == baseFields[b].name &&
            (fields[i].type == baseFields[b].type ||
             lower(fields[i].type) == lower(baseFields[b].type))) {
          placed[i] = true;
          order.push_back(i);
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
  for (unsigned i = 0; i < fields.size(); ++i)
    if (!placed[i])
      rest.push_back(i);
  llvm::stable_sort(rest, [&](unsigned a, unsigned b) {
    bool tagA = fields[a].type->isLiteral(), tagB = fields[b].type->isLiteral();
    if (tagA != tagB)
      return tagA;
    bool f = fields[a].type->nonOptional()->isFunction();
    bool g = fields[b].type->nonOptional()->isFunction();
    if (f != g)
      return g;
    uint64_t x = data.getABITypeAlign(lower(fields[a].type)).value();
    uint64_t y = data.getABITypeAlign(lower(fields[b].type)).value();
    return x != y ? x > y : fields[a].name < fields[b].name;
  });
  order.insert(order.end(), rest.begin(), rest.end());
  std::vector<llvm::Type *> elements{builder_.getInt64Ty(), builder_.getPtrTy()};
  layout.elements.resize(fields.size());
  for (unsigned i : order) {
    layout.elements[i] = unsigned(elements.size());
    elements.push_back(lower(fields[i].type));
  }
  layout.captures = unsigned(elements.size());
  if (const ClassDecl *cls = type->classDecl(); cls && cls->group)
    for (const VarSymbol *var : cls->group->captures)
      elements.push_back(var->boxed() ? builder_.getPtrTy() : lower(typeOf(var)));
  layout.type = llvm::StructType::get(ctx_, elements);

  auto *fnType = llvm::FunctionType::get(builder_.getVoidTy(), {builder_.getPtrTy()}, false);
  std::string name = layoutName(type);
  // The runtime makes regexes, and frees them (see TzRegex in runtime/tzrt.h);
  // so with formatters of dates, the other objects of Intl (which the
  // object's own layout frees, tz_intl_drop) and the objects of binary data.
  llvm::StringRef runtimeDrop = type->isRegExp()                         ? "tz_re_drop"
                                : type->isWeakRef()                      ? "tz_weak_drop"
                                : type->isDateFormat()                   ? "tz_dtf_drop"
                                : type->isIntl()                         ? "tz_intl_drop"
                                : type->isCrypto()                       ? "tz_crypto_drop"
                                : type->bytes() == Type::Bytes::ArrayBuffer ? "tz_abuf_drop"
                                : type->isByteView()                     ? "tz_bytes_drop"
                                : type->isBytes()                        ? "tz_bytes_object_drop"
                                                                         : "";
  if (!runtimeDrop.empty())
    layout.drop =
        cast<llvm::Function>(module_->getOrInsertFunction(runtimeDrop, fnType).getCallee());
  else
    layout.drop = llvm::Function::Create(fnType, llvm::GlobalValue::InternalLinkage,
                                         "tz." + name + ".drop", module_.get());
  layout.drop->addFnAttr(llvm::Attribute::NoUnwind);

  // The release: the count is the first word; the last reference drops the
  // object. Undefined (null) holds nothing.
  layout.release = llvm::Function::Create(fnType, llvm::GlobalValue::InternalLinkage,
                                          "tz." + name + ".release", module_.get());
  layout.release->addFnAttr(llvm::Attribute::AlwaysInline);
  layout.release->addFnAttr(llvm::Attribute::NoUnwind);
  llvm::IRBuilder<> b(llvm::BasicBlock::Create(ctx_, "entry", layout.release));
  auto *countedBlock = llvm::BasicBlock::Create(ctx_, "counted", layout.release);
  auto *dropBlock = llvm::BasicBlock::Create(ctx_, "drop", layout.release);
  auto *doneBlock = llvm::BasicBlock::Create(ctx_, "done", layout.release);
  llvm::Value *object = layout.release->getArg(0);
  b.CreateCondBr(b.CreateIsNotNull(object), countedBlock, doneBlock);
  b.SetInsertPoint(countedBlock);
  llvm::Value *left = b.CreateSub(b.CreateLoad(b.getInt64Ty(), object, "rc"), b.getInt64(1));
  b.CreateStore(left, object);
  b.CreateCondBr(b.CreateICmpEQ(left, b.getInt64(0)), dropBlock, doneBlock);
  b.SetInsertPoint(dropBlock);
  b.CreateCall(layout.drop, {object});
  b.CreateBr(doneBlock);
  b.SetInsertPoint(doneBlock);
  b.CreateRetVoid();

  if (runtimeDrop.empty())
    emitObjectDrop(type, layout);
  return layout;
}

// Releases what the fields hold, then frees the object. A list (`next?:
// Node`) would make that recursion as deep as the list is long, so a field
// that holds an object of the same type is released last, by going round
// again: freeing a long list takes no stack. If the field may hold objects of
// other types too (a class up the chain, or one with subclasses), it goes
// round again for the next object whose drop function is this one.
void CodeGen::emitObjectDrop(const Type *type, ObjectLayout &layout) {
  llvm::IRBuilderBase::InsertPoint point = builder_.saveIP();
  llvm::Function *drop = layout.drop;
  if (isCyclic(type)) {
    emitMemberDrop(type, layout);
    builder_.restoreIP(point);
    return;
  }
  auto *entry = llvm::BasicBlock::Create(ctx_, "entry", drop);
  auto *loopBlock = llvm::BasicBlock::Create(ctx_, "object", drop);
  builder_.SetInsertPoint(entry);
  builder_.CreateBr(loopBlock);
  builder_.SetInsertPoint(loopBlock);
  llvm::PHINode *object = builder_.CreatePHI(builder_.getPtrTy(), 2, "object");
  object->addIncoming(drop->getArg(0), entry);
  if (isWeakTarget(type))
    forgetWeakRefs(object);

  llvm::ArrayRef<Field> fields = type->fields();
  int next = -1; // the field of the same type, released last
  for (unsigned i = 0; i < fields.size(); ++i)
    if (const Type *t = fields[i].type->nonOptional();
        t->isObject() && type->isSubtypeOf(t) && !refMayBeMember(t))
      next = int(i);
  bool exact =
      next >= 0 && fields[unsigned(next)].type->nonOptional() == type && !hasSubtypes(type);
  std::vector<std::pair<llvm::Value *, const Type *>> values;
  llvm::Value *nextValue = nullptr;
  for (unsigned i = 0; i < fields.size(); ++i) {
    const Type *fieldType = fields[i].type;
    if (!fieldType->isManaged())
      continue;
    llvm::Value *address = builder_.CreateStructGEP(layout.type, object, layout.elements[i]);
    llvm::Value *value = builder_.CreateLoad(lower(fieldType), address, fields[i].name);
    if (debugMemory_) {
      llvm::Function *saved = function_;
      function_ = drop;
      checkOwned(type, fields[i].name, value, fieldType);
      function_ = saved;
    }
    if (int(i) == next)
      nextValue = value;
    else
      values.push_back({value, fieldType});
  }
  // The variables a literal's methods captured.
  std::vector<llvm::Value *> boxes;
  if (const ClassDecl *cls = type->classDecl(); cls && cls->group) {
    const std::vector<VarSymbol *> &captures = cls->group->captures;
    for (size_t i = 0; i < captures.size(); ++i) {
      const VarSymbol *var = captures[i];
      llvm::Value *address =
          builder_.CreateStructGEP(layout.type, object, unsigned(layout.captures + i));
      if (var->boxed()) {
        boxes.push_back(builder_.CreateLoad(builder_.getPtrTy(), address, var->name + ".box"));
      } else if (typeOf(var)->isManaged()) {
        const Type *varType = typeOf(var);
        values.push_back({builder_.CreateLoad(lower(varType), address, var->name), varType});
      }
    }
  }
  builder_.CreateCall(rt_.objFree, {object});
  for (auto [value, fieldType] : values)
    releaseValue(value, fieldType);
  for (llvm::Value *box : boxes)
    builder_.CreateCall(objReleaseFn_, {box});
  if (nextValue) {
    auto *countedBlock = llvm::BasicBlock::Create(ctx_, "next", drop);
    auto *doneBlock = llvm::BasicBlock::Create(ctx_, "done", drop);
    builder_.CreateCondBr(builder_.CreateIsNotNull(nextValue), countedBlock, doneBlock);
    builder_.SetInsertPoint(countedBlock);
    llvm::Value *left = builder_.CreateSub(builder_.CreateLoad(builder_.getInt64Ty(), nextValue, "rc"),
                                           builder_.getInt64(1));
    builder_.CreateStore(left, nextValue);
    if (exact) {
      object->addIncoming(nextValue, countedBlock);
      builder_.CreateCondBr(builder_.CreateICmpEQ(left, builder_.getInt64(0)), loopBlock,
                            doneBlock);
    } else {
      // The drop function of the next object's own descriptor.
      auto *deadBlock = llvm::BasicBlock::Create(ctx_, "next.dead", drop);
      auto *otherBlock = llvm::BasicBlock::Create(ctx_, "next.other", drop);
      builder_.CreateCondBr(builder_.CreateICmpEQ(left, builder_.getInt64(0)), deadBlock,
                            doneBlock);
      builder_.SetInsertPoint(deadBlock);
      llvm::Value *descriptor = builder_.CreateLoad(
          builder_.getPtrTy(),
          builder_.CreateConstInBoundsGEP1_64(builder_.getInt8Ty(), nextValue, 8));
      llvm::Value *nextDrop = builder_.CreateLoad(builder_.getPtrTy(), descriptor, "drop");
      object->addIncoming(nextValue, deadBlock);
      builder_.CreateCondBr(builder_.CreateICmpEQ(nextDrop, drop), loopBlock, otherBlock);
      // Another type's drop, as a tail call even without optimizations: a
      // list of objects of mixed types takes no stack either.
      builder_.SetInsertPoint(otherBlock);
      llvm::CallInst *call = builder_.CreateCall(
          llvm::FunctionType::get(builder_.getVoidTy(), {builder_.getPtrTy()}, false), nextDrop,
          {nextValue});
      call->setTailCallKind(llvm::CallInst::TCK_MustTail);
      builder_.CreateRetVoid();
    }
    builder_.SetInsertPoint(doneBlock);
  }
  builder_.CreateRetVoid();
  builder_.restoreIP(point);
}

llvm::Constant *CodeGen::layoutDescriptor(const Type *type, llvm::ArrayRef<unsigned> keys) {
  // TZ_LAYOUT_TUPLE, TZ_LAYOUT_MATCH and TZ_LAYOUT_DATE (runtime/tzrt.h).
  return layoutDescriptor(type, keys,
                          type->isTuple() ? 1 : type->isMatch() ? 64 : type->isDate() ? 1024 : 0);
}

llvm::Constant *CodeGen::layoutDescriptor(const Type *type, llvm::ArrayRef<unsigned> keys,
                                          uint32_t flags) {
  std::vector<unsigned> key(keys.begin(), keys.end());
  key.push_back(~flags); // apart from the keys, which are field positions
  llvm::Constant *&descriptor = layoutDescriptors_[{type, std::move(key)}];
  if (descriptor)
    return descriptor;
  const ObjectLayout &layout = objectLayout(type);
  const llvm::StructLayout *offsets = module_->getDataLayout().getStructLayout(layout.type);
  std::vector<llvm::Constant *> entries;
  for (unsigned k : keys) {
    const Field &field = type->fields()[k];
    uint64_t offset = offsets->getElementOffset(layout.elements[k]);
    entries.push_back(llvm::ConstantStruct::get(
        fieldDescTy_, {utf8Constant(field.name), builder_.getInt32(uint32_t(field.name.size())),
                       builder_.getInt32(uint32_t(offset)), typeDescriptor(field.type)}));
  }
  llvm::Constant *fields = llvm::ConstantPointerNull::get(builder_.getPtrTy());
  if (!entries.empty()) {
    auto *arrayType = llvm::ArrayType::get(fieldDescTy_, entries.size());
    auto *global = new llvm::GlobalVariable(*module_, arrayType, /*isConstant=*/true,
                                            llvm::GlobalValue::PrivateLinkage,
                                            llvm::ConstantArray::get(arrayType, entries),
                                            "tz.fields." + layoutName(type));
    global->setUnnamedAddr(llvm::GlobalValue::UnnamedAddr::Global);
    fields = global;
  }
  // The number that tells objects of this type apart from those of the
  // other members of a union (see codegen_union.cpp). A WeakRef prints as
  // node prints one, `WeakRef {}`.
  llvm::Constant *name = type->isWeakRef() ? utf8Constant("WeakRef")
                                           : llvm::ConstantPointerNull::get(builder_.getPtrTy());
  llvm::Constant *init = llvm::ConstantStruct::get(
      layoutDescTy_, {layout.drop, fields, builder_.getInt32(uint32_t(entries.size())),
                      builder_.getInt32(flags), name,
                      builder_.getInt32(type->isWeakRef() ? 7 : 0),
                      builder_.getInt32(type->isClass() ? 0 : plainTypeId(type))});
  // An object of a type that implements interfaces with tables: then the
  // tables (see codegen_inherit.cpp).
  llvm::Constant *itables = itablesOf(type);
  if (!itables->isNullValue()) {
    auto *descType = llvm::StructType::get(ctx_, {layoutDescTy_, builder_.getPtrTy()});
    init = llvm::ConstantStruct::get(descType, {init, itables});
  }
  auto *global = new llvm::GlobalVariable(*module_, init->getType(), /*isConstant=*/true,
                                          llvm::GlobalValue::PrivateLinkage, init,
                                          "tz.layout." + layoutName(type));
  global->setUnnamedAddr(llvm::GlobalValue::UnnamedAddr::Global);
  descriptor = global;
  return descriptor;
}

llvm::Value *CodeGen::fieldAddress(llvm::Value *object, const Type *type, llvm::StringRef name,
                                   bool exact) {
  if (type->isUnion()) // in each member, where it is there (codegen_union.cpp)
    return unionFieldAddress(object, type, name);
  // Through an interface whose implementers put the field elsewhere: at the
  // offset the object's table gives.
  if (const InterfaceMember *member = exact ? nullptr : dispatchedMember(type, name)) {
    assert(!member->callable && "a method is called or bound, not read");
    llvm::Value *offset = builder_.CreateLoad(builder_.getInt64Ty(),
                                              tableEntry(object, type, unsigned(member->entry)),
                                              name + ".offset");
    return builder_.CreateInBoundsGEP(builder_.getInt8Ty(), object, offset, name + ".addr");
  }
  const ObjectLayout &layout = objectLayout(type);
  return builder_.CreateStructGEP(layout.type, object, layout.elements[fieldIndex(type, name)], name + ".addr");
}

llvm::Value *CodeGen::allocObject(const Type *type, llvm::ArrayRef<unsigned> keys) {
  return allocObject(type, keys, isCyclic(type));
}

llvm::Value *CodeGen::allocObject(const Type *type, llvm::ArrayRef<unsigned> keys, bool member) {
  const ObjectLayout &layout = objectLayout(type);
  llvm::Constant *descriptor = layoutDescriptor(type, keys);
  uint64_t size = module_->getDataLayout().getTypeAllocSize(layout.type);
  llvm::Value *object;
  if (member) {
    object = allocMember(size, "object"); // counted by its group
  } else {
    object = builder_.CreateCall(rt_.objAlloc, {builder_.getInt64(size)}, "object");
    builder_.CreateStore(builder_.getInt64(1), builder_.CreateStructGEP(layout.type, object, 0));
  }
  builder_.CreateStore(descriptor, builder_.CreateStructGEP(layout.type, object, 1));
  return object;
}

// `{ x: 1, y }`: the values in the order written, then undefined in the
// optional fields the literal leaves out.
llvm::Value *CodeGen::emitObjectLiteral(ObjectLitExpr *lit) {
  if (lit->type->isJson())
    return emitJsonObjectLiteral(lit);
  if (lit->type->isRecord())
    return emitRecordLiteral(lit);
  if (lit->cls)
    return emitMethodLiteral(lit);
  if (lit->hasSpread())
    return emitSpreadLiteral(lit);
  const Type *type = lit->type;
  const ObjectLayout &layout = objectLayout(type);
  std::vector<unsigned> keys;
  for (const ObjectLitExpr::Property &prop : lit->props)
    keys.push_back(fieldIndex(type, prop.name));
  for (unsigned i = 0; i < type->fields().size(); ++i)
    if (!llvm::is_contained(keys, i))
      keys.push_back(i);
  // If a value may throw, the values come first (held, see emitHeldValues),
  // then the object: one that a throw left half filled could not be
  // released.
  std::vector<Expr *> exprs;
  for (const ObjectLitExpr::Property &prop : lit->props)
    exprs.push_back(prop.value);
  std::vector<llvm::Value *> values;
  if (llvm::any_of(exprs, [&](Expr *e) { return exprThrows(e); }))
    values = emitHeldValues(exprs);
  bool member = makesMember(lit, type);
  llvm::Value *object = allocObject(type, keys, member);
  noteUnstored(object, lit);
  // Nothing can reach the new object while its values are evaluated.
  for (size_t k = 0; k < keys.size(); ++k) {
    unsigned i = keys[k];
    llvm::Value *value = k < values.size()          ? values[k]
                         : k < lit->props.size() ? emitExpr(lit->props[k].value)
                                                 : undefinedValue(type->fields()[i].type);
    builder_.CreateStore(value, builder_.CreateStructGEP(layout.type, object, layout.elements[i]));
    if (member)
      groupStore(object, value, type->fields()[i].type);
  }
  return object;
}

// `{ ...a, x: 1, ...b }`. The object is made first with every field empty
// (zero, which releases as nothing), and each part stores its values in
// order, replacing what an earlier part stored. A spread reads the fields of
// its object when it comes, as in JavaScript, but does not copy an optional
// field that holds undefined. The keys are in the order the parts first give
// them, the fields of a spread object in the order of its type.
llvm::Value *CodeGen::emitSpreadLiteral(ObjectLitExpr *lit) {
  const Type *type = lit->type;
  llvm::ArrayRef<Field> fields = type->fields();
  std::vector<unsigned> keys;
  auto addKey = [&](llvm::StringRef name) {
    unsigned i = fieldIndex(type, name);
    if (!llvm::is_contained(keys, i))
      keys.push_back(i);
  };
  for (const ObjectLitExpr::Property &prop : lit->props) {
    if (!prop.spread) {
      addKey(prop.name);
      continue;
    }
    for (const Field &f : prop.value->type->fields())
      if (!llvm::is_contained(prop.omit, f.name))
        addKey(f.name);
  }
  for (unsigned i = 0; i < fields.size(); ++i)
    if (!llvm::is_contained(keys, i))
      keys.push_back(i);

  llvm::Value *object = allocObject(type, keys, makesMember(lit, type));
  noteUnstored(object, lit);
  for (const Field &f : fields)
    builder_.CreateStore(llvm::Constant::getNullValue(lower(f.type)),
                         fieldAddress(object, type, f.name, /*exact=*/true));
  // Nothing can reach the new object while the parts are evaluated; a part
  // that throws leaves it with what is stored so far, which the way up
  // releases (every field is a value or empty).
  Held held = hold(object, type, llvm::any_of(lit->props, [&](const ObjectLitExpr::Property &p) {
                     return exprThrows(p.value);
                   }));
  for (const ObjectLitExpr::Property &prop : lit->props) {
    if (!prop.spread) {
      if (prop.replaced) {
        emitDiscarded(prop.value);
        continue;
      }
      const Type *to = type->field(prop.name)->type;
      storeIn(object, type, fieldAddress(object, type, prop.name, /*exact=*/true),
              emitExpr(prop.value), to);
      continue;
    }
    const Type *source = prop.value->type;
    // Its fields are copied right away, so a variable can be borrowed.
    Operand from = emitOperand(prop.value, /*canBorrow=*/true);
    for (const Field &f : source->fields()) {
      if (llvm::is_contained(prop.omit, f.name) || llvm::is_contained(prop.replacedFields, f.name))
        continue;
      const Type *to = type->field(f.name)->type;
      llvm::Value *address = fieldAddress(object, type, f.name, /*exact=*/true);
      // A method of an interface that some class implements: bound to the
      // object.
      if (const InterfaceMember *method = dispatchedMember(source, f.name);
          method && method->callable) {
        llvm::Value *info = builder_.CreateLoad(
            builder_.getPtrTy(), tableEntry(from.value, source, unsigned(method->entry + 1)));
        retainValue(from.value, source);
        storeIn(object, type, address, makeFunctionValue(info, from.value), to);
        continue;
      }
      llvm::Value *value =
          builder_.CreateLoad(lower(f.type), fieldAddress(from.value, source, f.name), f.name);
      if (!f.type->isOptional()) {
        retainValue(value, f.type);
        storeIn(object, type, address, emitConversion(value, f.type, to), to);
        continue;
      }
      llvm::BasicBlock *copyBlock = newBlock("spread.copy");
      llvm::BasicBlock *nextBlock = newBlock("spread.next");
      builder_.CreateCondBr(optionalHas(value, f.type), copyBlock, nextBlock);
      switchTo(copyBlock);
      const Type *inner = f.type->inner();
      llvm::Value *present = optionalValue(value, f.type);
      retainValue(present, inner);
      storeIn(object, type, address, emitConversion(present, inner, to), to);
      builder_.CreateBr(nextBlock);
      switchTo(nextBlock);
    }
    dropOperand(from);
  }
  return take(held);
}

llvm::Value *CodeGen::emitTupleLiteral(ArrayLitExpr *lit) {
  const Type *type = lit->type;
  std::vector<unsigned> keys(type->fields().size());
  std::iota(keys.begin(), keys.end(), 0u);
  // As for an object literal: if an element may throw, the elements come
  // first (held), then the tuple, which a throw would leave half filled.
  std::vector<Expr *> exprs;
  for (const ArrayLitExpr::Element &e : lit->elements)
    exprs.push_back(e.expr);
  std::vector<llvm::Value *> values;
  if (llvm::any_of(exprs, [&](Expr *e) { return exprThrows(e); }))
    values = emitHeldValues(exprs);
  bool member = makesMember(lit, type);
  llvm::Value *object = allocObject(type, keys, member);
  noteUnstored(object, lit);
  for (size_t i = 0; i < exprs.size(); ++i) {
    llvm::Value *value = i < values.size() ? values[i] : emitExpr(exprs[i]);
    builder_.CreateStore(value, fieldAddress(object, type, type->fields()[i].name));
    if (member)
      groupStore(object, value, type->fields()[i].type);
  }
  return object;
}

void CodeGen::emitTupleSpread(llvm::Value *array, llvm::Value *tuple, const Type *tupleType,
                              const Type *element) {
  for (const Field &f : tupleType->fields()) {
    llvm::Value *value = builder_.CreateLoad(lower(f.type), fieldAddress(tuple, tupleType, f.name));
    retainValue(value, f.type);
    emitPush(array, {emitConversion(value, f.type, element)}, element);
  }
}

// Object.keys, Object.values and Object.entries: the runtime walks the
// fields in the order of the object's keys, leaving out the optional ones
// that hold undefined.
llvm::Value *CodeGen::emitObjectCall(CallExpr *call) {
  if (call->args[0]->type->nonOptional()->isJson())
    return emitJsonObjectCall(call);
  if (call->args[0]->type->isDict())
    return emitDictObjectCall(call);
  Operand object = emitOperand(call->args[0], /*canBorrow=*/true);
  const Type *element = call->type->inner();
  llvm::Value *result;
  if (call->builtin == Builtin::ObjectKeys) {
    result = builder_.CreateCall(rt_.objKeys, {object.value, typeDescriptor(element)}, "keys");
  } else if (call->builtin == Builtin::ObjectValues) {
    result = builder_.CreateCall(rt_.objValues, {object.value, typeDescriptor(element)}, "values");
  } else {
    // Each entry is a new tuple [key, value].
    uint64_t size = module_->getDataLayout().getTypeAllocSize(objectLayout(element).type);
    result = builder_.CreateCall(rt_.objEntries,
                                 {object.value, typeDescriptor(element),
                                  layoutDescriptor(element, {0, 1}), builder_.getInt64(size)},
                                 "entries");
  }
  dropOperand(object);
  return result;
}

llvm::Value *CodeGen::emitFieldRead(MemberExpr *member) {
  if (member->object->type->isUnion()) // a field each member has
    return emitUnionFieldRead(member);
  // A method of an interface, as a value: bound to the object, with the info
  // of the code the object's table gives.
  if (member->signature >= 0 &&
      !dispatchedMember(member->object->type, member->name, member->signature)) {
    // Of an interface that no class implements (see emitInterfaceCall).
    emitDiscarded(member->object);
    builder_.CreateCall(
        llvm::Intrinsic::getOrInsertDeclaration(module_.get(), llvm::Intrinsic::trap));
    return llvm::PoisonValue::get(fnTy_);
  }
  if (const InterfaceMember *method =
          dispatchedMember(member->object->type, member->name, member->signature);
      method && method->callable) {
    llvm::Value *object = emitExpr(member->object);
    llvm::Value *info = builder_.CreateLoad(
        builder_.getPtrTy(), tableEntry(object, member->object->type, unsigned(method->entry + 1)),
        member->name + ".info");
    return makeFunctionValue(info, object);
  }
  Operand object = emitOperand(member->object, /*canBorrow=*/true);
  llvm::Value *value = builder_.CreateLoad(lower(member->type),
                                           fieldAddress(object.value, object.type, member->name), member->name);
  retainValue(value, member->type);
  dropOperand(object);
  return value;
}

// `o.x = v` and `o.x op= v`. As in JavaScript, the object is evaluated
// first, and the old value of a compound assignment is read before v.
llvm::Value *CodeGen::keyedFieldAddress(llvm::Value *object, const Type *type, llvm::Value *key,
                                        const Type *keyType) {
  llvm::ArrayRef<const Type *> names =
      keyType->isUnion() ? keyType->members() : llvm::ArrayRef<const Type *>(keyType);
  llvm::Value *address = nullptr;
  for (const Type *n : llvm::reverse(names)) {
    std::string name;
    const std::u16string &text = n->literalText();
    llvm::convertUTF16ToUTF8String(
        llvm::ArrayRef<llvm::UTF16>(reinterpret_cast<const llvm::UTF16 *>(text.data()), text.size()),
        name);
    llvm::Value *field = fieldAddress(object, type, name);
    address = address ? builder_.CreateSelect(
                            builder_.CreateICmpEQ(key, builder_.getInt32(n->atom())), field, address)
                      : field;
  }
  return address;
}

llvm::Value *CodeGen::emitFieldAssign(AssignExpr *assign, bool needResult) {
  // `o[k] = v` with a key of literals, or `o.name = v`.
  auto *index = dyn_cast<IndexExpr>(assign->target);
  auto *member = dyn_cast<MemberExpr>(assign->target);
  const Type *type = index ? index->type : member->type;
  Expr *objectExpr = index ? index->object : member->object;
  bool calm = cannotAssign(assign->value) && (!index || cannotAssign(index->index));
  Operand object = emitOperand(objectExpr, calm);
  llvm::Value *address =
      index ? keyedFieldAddress(object.value, object.type, emitExpr(index->index),
                                index->index->type)
            : fieldAddress(object.value, object.type, member->name);
  llvm::Value *value;
  if (!assign->op) {
    value = emitExpr(assign->value);
  } else if (type->isString()) {
    llvm::Value *old = builder_.CreateLoad(strTy_, address);
    retainString(old); // the right side may replace the field
    Operand rhs = emitConcatPart(assign->value, /*canBorrow=*/false);
    value = concatOperands({{old, true, type}, rhs});
  } else if (type->isDecimal()) {
    llvm::Value *old = builder_.CreateLoad(lower(type), address);
    retainValue(old, type); // the right side may replace the field
    value = emitDecimalCompound(*assign->op, old, assign->value);
    releaseValue(old, type);
  } else {
    llvm::Value *old = builder_.CreateLoad(lower(type), address);
    value = emitArithmetic(*assign->op, old, emitExpr(assign->value), type);
  }
  // Debug mode notes the field a store into a member writes.
  storeField_ = index ? index->field.str() : member->name.str();
  storeLoc_ = assign->loc();
  storeIn(object.value, object.type, address, value, type);
  storeField_.clear();
  storeLoc_ = llvm::SMLoc();
  dropOperand(object);
  if (!needResult)
    return nullptr;
  retainValue(value, type);
  return value;
}

} // namespace tz
