// Unions and literal types (docs/objects.md, section 7).
//
// How a value lies in memory follows from the slots its type takes (see
// UnionSlot in src/sema/types.h):
//   - a string literal type, or a union of them, is an i32: the literal's
//     atom, its number among the string literals of the program (0 is
//     undefined). Comparing is comparing numbers; the text comes from a
//     constant table of the strings, without allocating;
//   - a number or boolean literal type, or a union of them, is a value of its
//     base type: `0 | 1` is an int;
//   - a union of objects is a reference to the object, null for undefined;
//     which member it holds, its descriptor tells (the class number, or a
//     number of its own for any other object type);
//   - any other union holds a tag, the number of its slot (the same in every
//     union, 0 for undefined), and the value of that slot in one or two
//     words: { i32 tag, i64 word, i64 word }, 16 or 24 bytes, never on the
//     heap. A slot of objects holds a reference as above.

#include "codegen/codegen.h"

#include "llvm/IR/MDBuilder.h"
#include "llvm/Support/ConvertUTF.h"
#include "llvm/Support/ErrorHandling.h"

using llvm::cast;
using llvm::dyn_cast;
using llvm::isa;

namespace tz {

namespace {

// The words a value of a slot takes in a tagged union.
unsigned slotWords(UnionSlot slot) {
  switch (slot) {
  case UnionSlot::String:
  case UnionSlot::Decimal:
  case UnionSlot::Function: return 2;
  default: return 1;
  }
}

// The descriptor of an object, whose class number tells its type.
constexpr unsigned kDescriptorClass = 6;

// A value held as a pointer, null for undefined: see isReference in
// codegen.cpp.
bool isReferenceLike(const Type *type) {
  return type->isObject() || type->isIterator() || type->isJson() || type->isDict() ||
         type->isPromise() || type->isUnknown() || type->isClassRef() ||
         (type->isUnion() && !type->isTagged() && type->slots().front() == UnionSlot::Object);
}

} // namespace

bool CodeGen::isAtomType(const Type *type) {
  return type->isStringLiteral() ||
         (type->isUnion() && !type->isTagged() && type->slots().front() == UnionSlot::Atom);
}

bool CodeGen::isObjectUnion(const Type *type) {
  return type->isUnion() && !type->isTagged() && type->slots().front() == UnionSlot::Object;
}

const Type *CodeGen::storedType(const Type *type) {
  if (type->isOptional()) {
    const Type *inner = storedType(type->inner());
    return inner == type->inner() ? type : Type::optional(inner);
  }
  // A number or boolean literal, or a union of them, is its base.
  if (type->isLiteral() && !type->isStringLiteral())
    return type->literalBase();
  if (type->isUnion() && !type->isTagged()) {
    UnionSlot slot = type->slots().front();
    if (slot == UnionSlot::Int)
      return Type::intTy();
    if (slot == UnionSlot::Double)
      return Type::doubleTy();
    if (slot == UnionSlot::Bool)
      return Type::boolTy();
  }
  return type;
}

bool CodeGen::zeroIsUndefined(const Type *inner) {
  return isReferenceLike(inner) || isAtomType(inner) || inner->isTagged();
}

llvm::Type *CodeGen::lowerUnion(const Type *type) {
  if (isAtomType(type))
    return builder_.getInt32Ty();
  if (isObjectUnion(type))
    return builder_.getPtrTy();
  if (const Type *stored = storedType(type); stored != type)
    return lower(stored);
  unsigned words = 1;
  for (UnionSlot slot : type->slots())
    words = std::max(words, slotWords(slot));
  llvm::Type *i64 = builder_.getInt64Ty();
  return words == 1 ? llvm::StructType::get(ctx_, {builder_.getInt32Ty(), i64})
                    : llvm::StructType::get(ctx_, {builder_.getInt32Ty(), i64, i64});
}

//===----------------------------------------------------------------------===//
// Atoms
//===----------------------------------------------------------------------===//

llvm::GlobalVariable *CodeGen::atomTable() {
  if (atomTable_)
    return atomTable_;
  // Index 0 is undefined; then the string literals of the program by atom.
  std::vector<llvm::Constant *> strings{stringLiteral(u"undefined")};
  for (const Type *literal : Type::stringLiterals())
    strings.push_back(stringLiteral(literal->literalText()));
  auto *arrayType = llvm::ArrayType::get(strTy_, strings.size());
  atomTable_ = new llvm::GlobalVariable(*module_, arrayType, /*isConstant=*/true,
                                        llvm::GlobalValue::PrivateLinkage,
                                        llvm::ConstantArray::get(arrayType, strings), "tz.atoms");
  return atomTable_;
}

llvm::Value *CodeGen::atomString(llvm::Value *atom) {
  if (auto *constant = dyn_cast<llvm::ConstantInt>(atom)) {
    uint64_t index = constant->getZExtValue();
    llvm::ArrayRef<const Type *> literals = Type::stringLiterals();
    return index == 0 || index > literals.size()
               ? stringLiteral(u"undefined")
               : stringLiteral(literals[index - 1]->literalText());
  }
  llvm::GlobalVariable *table = atomTable();
  llvm::Value *address = builder_.CreateInBoundsGEP(
      table->getValueType(), table,
      {builder_.getInt64(0), builder_.CreateZExt(atom, builder_.getInt64Ty())});
  return builder_.CreateLoad(strTy_, address, "atom.text");
}

llvm::Constant *CodeGen::atomDescriptor() {
  if (atomDescriptor_)
    return atomDescriptor_;
  auto *descType = llvm::StructType::get(ctx_, {typeDescTy_, builder_.getPtrTy()});
  llvm::Constant *base =
      llvm::ConstantStruct::get(typeDescTy_, {builder_.getInt32(15), builder_.getInt32(4),
                                              llvm::ConstantPointerNull::get(builder_.getPtrTy())});
  auto *global = new llvm::GlobalVariable(
      *module_, descType, /*isConstant=*/true, llvm::GlobalValue::PrivateLinkage,
      llvm::ConstantStruct::get(descType, {base, atomTable()}), "tz.type.atom");
  atomDescriptor_ = global;
  return global;
}

llvm::Constant *CodeGen::unionDescriptor(const Type *type) {
  if (isAtomType(type))
    return atomDescriptor();
  if (isObjectUnion(type))
    return typeDescriptor(Type::anyObject());
  if (const Type *stored = storedType(type); stored != type)
    return typeDescriptor(stored);
  if (llvm::Constant *found = typeDescriptors_.lookup(type))
    return found;
  // { TzType, slots[12] }: TzUnionType in runtime/tzrt.h.
  constexpr unsigned kSlots = 12;
  auto *slotsType = llvm::ArrayType::get(builder_.getPtrTy(), kSlots);
  std::vector<llvm::Constant *> slots(kSlots, llvm::ConstantPointerNull::get(builder_.getPtrTy()));
  for (UnionSlot slot : type->slots())
    slots[unsigned(slot)] = typeDescriptor(type->slotPart(slot));
  uint64_t size = module_->getDataLayout().getTypeAllocSize(lower(type));
  llvm::Constant *base = llvm::ConstantStruct::get(
      typeDescTy_, {builder_.getInt32(14), builder_.getInt32(uint32_t(size)),
                    llvm::ConstantPointerNull::get(builder_.getPtrTy())});
  auto *descType = llvm::StructType::get(ctx_, {typeDescTy_, slotsType});
  auto *global = new llvm::GlobalVariable(
      *module_, descType, /*isConstant=*/true, llvm::GlobalValue::PrivateLinkage,
      llvm::ConstantStruct::get(descType, {base, llvm::ConstantArray::get(slotsType, slots)}),
      "tz.type." + type->name());
  global->setUnnamedAddr(llvm::GlobalValue::UnnamedAddr::Global);
  typeDescriptors_[type] = global;
  return global;
}

//===----------------------------------------------------------------------===//
// Tagged values
//===----------------------------------------------------------------------===//

llvm::Value *CodeGen::unionTag(llvm::Value *value) {
  return builder_.CreateExtractValue(value, 0, "tag");
}

// A scalar as a word, and back.
static llvm::Value *toWord(llvm::IRBuilder<> &b, llvm::Value *v) {
  llvm::Type *type = v->getType();
  if (type->isPointerTy())
    return b.CreatePtrToInt(v, b.getInt64Ty());
  if (type->isDoubleTy())
    return b.CreateBitCast(v, b.getInt64Ty());
  if (type->isIntegerTy(64))
    return v;
  return b.CreateZExt(v, b.getInt64Ty());
}

static llvm::Value *fromWord(llvm::IRBuilder<> &b, llvm::Value *word, llvm::Type *type) {
  if (type->isPointerTy())
    return b.CreateIntToPtr(word, type);
  if (type->isDoubleTy())
    return b.CreateBitCast(word, type);
  if (type->isIntegerTy(64))
    return word;
  return b.CreateTrunc(word, type);
}

llvm::Value *CodeGen::makeTagged(const Type *unionType, UnionSlot slot, llvm::Value *part) {
  auto *type = cast<llvm::StructType>(lower(unionType));
  llvm::Value *result = llvm::Constant::getNullValue(type);
  result = builder_.CreateInsertValue(result, builder_.getInt32(unsigned(slot)), 0);
  std::vector<llvm::Value *> words;
  if (auto *structType = dyn_cast<llvm::StructType>(part->getType())) {
    for (unsigned i = 0; i < structType->getNumElements(); ++i)
      words.push_back(toWord(builder_, builder_.CreateExtractValue(part, i)));
  } else {
    words.push_back(toWord(builder_, part));
  }
  for (unsigned i = 0; i < words.size(); ++i)
    result = builder_.CreateInsertValue(result, words[i], i + 1);
  return result;
}

llvm::Value *CodeGen::taggedPart(llvm::Value *value, const Type *unionType, UnionSlot slot) {
  llvm::Type *type = lower(unionType->slotPart(slot));
  if (auto *structType = dyn_cast<llvm::StructType>(type)) {
    llvm::Value *part = llvm::UndefValue::get(type);
    for (unsigned i = 0; i < structType->getNumElements(); ++i)
      part =
          builder_.CreateInsertValue(part,
                                     fromWord(builder_, builder_.CreateExtractValue(value, i + 1),
                                              structType->getElementType(i)),
                                     i);
    return part;
  }
  return fromWord(builder_, builder_.CreateExtractValue(value, 1), type);
}

llvm::Value *CodeGen::forEachSlot(llvm::Value *value, llvm::ArrayRef<UnionSlot> slots,
                                  llvm::function_ref<llvm::Value *(UnionSlot)> each,
                                  llvm::Type *resultType, llvm::Value *otherwise) {
  llvm::BasicBlock *endBlock = newBlock("union.end");
  llvm::BasicBlock *otherBlock = newBlock("union.other");
  llvm::SwitchInst *sw = builder_.CreateSwitch(unionTag(value), otherBlock, unsigned(slots.size()));
  llvm::PHINode *phi = nullptr;
  std::vector<std::pair<llvm::Value *, llvm::BasicBlock *>> incoming;
  for (UnionSlot slot : slots) {
    llvm::BasicBlock *block = newBlock("union.slot");
    sw->addCase(builder_.getInt32(unsigned(slot)), block);
    switchTo(block);
    llvm::Value *result = each(slot);
    if (blockIsOpen()) {
      incoming.push_back({result, builder_.GetInsertBlock()});
      builder_.CreateBr(endBlock);
    }
  }
  switchTo(otherBlock);
  if (otherwise)
    incoming.push_back({otherwise, otherBlock});
  builder_.CreateBr(endBlock);
  switchTo(endBlock);
  if (resultType && !resultType->isVoidTy()) {
    phi = builder_.CreatePHI(resultType, unsigned(incoming.size()));
    for (auto [v, block] : incoming)
      phi->addIncoming(v ? v : llvm::PoisonValue::get(resultType), block);
  }
  return phi;
}

void CodeGen::countTagged(llvm::Value *value, const Type *type, bool retain) {
  std::vector<UnionSlot> managed;
  for (UnionSlot slot : type->slots())
    if (type->slotPart(slot)->isManaged())
      managed.push_back(slot);
  if (managed.empty())
    return;
  llvm::Function *&helper = (retain ? unionRetains_ : unionReleases_)[type];
  if (!helper) {
    // One function for each union, which code calls: the switch is small.
    auto *fnType = llvm::FunctionType::get(builder_.getVoidTy(), {lower(type)}, false);
    helper = llvm::Function::Create(
        fnType, llvm::GlobalValue::InternalLinkage,
        (retain ? "tz.union.retain." : "tz.union.release.") + type->name(), module_.get());
    helper->addFnAttr(llvm::Attribute::NoUnwind);
    llvm::IRBuilder<>::InsertPointGuard guard(builder_);
    llvm::Function *outer = function_;
    function_ = helper;
    builder_.SetInsertPoint(llvm::BasicBlock::Create(ctx_, "entry", helper));
    llvm::Value *arg = helper->getArg(0);
    forEachSlot(
        arg, managed,
        [&](UnionSlot slot) -> llvm::Value * {
          llvm::Value *part = taggedPart(arg, type, slot);
          if (retain)
            retainValue(part, type->slotPart(slot));
          else
            releaseValue(part, type->slotPart(slot));
          return nullptr;
        },
        nullptr, nullptr);
    builder_.CreateRetVoid();
    function_ = outer;
  }
  builder_.CreateCall(helper, {value});
}

//===----------------------------------------------------------------------===//
// Conversions
//===----------------------------------------------------------------------===//

// The slot of `to`, a tagged union, that a part of a union in `slot` goes to
// when `to` has no such slot: a string literal is a string, an int widens.
// None for a part that a check ruled out (`to` is a narrowed type).
static UnionSlot targetSlot(UnionSlot slot, const Type *to) {
  auto has = [&](UnionSlot s) { return llvm::is_contained(to->slots(), s); };
  if (has(slot))
    return slot;
  if (slot == UnionSlot::Atom && has(UnionSlot::String))
    return UnionSlot::String;
  if (slot == UnionSlot::Int)
    for (UnionSlot s : {UnionSlot::Long, UnionSlot::Double, UnionSlot::Decimal})
      if (has(s))
        return s;
  if (slot == UnionSlot::Long && has(UnionSlot::Double))
    return UnionSlot::Double;
  return UnionSlot::None;
}

llvm::Value *CodeGen::emitUnionConversion(llvm::Value *value, const Type *from, const Type *to) {
  if (from == to)
    return value;
  if (to->isOptional()) {
    if (from->isUndefined())
      return undefinedValue(to);
    if (from->isOptional())
      return makeOptional(emitConversion(optionalValue(value, from), from->inner(), to->inner()),
                          optionalHas(value, from), to);
    return makeOptional(emitConversion(value, from, to->inner()), builder_.getTrue(), to);
  }
  if (from->isOptional()) // narrowed: known to hold a value
    return emitConversion(optionalValue(value, from), from->inner(), to);

  // The text of a string literal: a constant, which is not counted.
  if (isAtomType(from) && (to->isString() || to->isJson()))
    return emitConversion(atomString(value), Type::string(), to);
  // A Json that checks showed to hold one of the members.
  if (from->isJson() && to->isTagged()) {
    llvm::Value *kind = builder_.CreateCall(
        jsonFunction("tz_json_typeof", builder_.getInt32Ty(), {builder_.getPtrTy()}), {value});
    llvm::BasicBlock *endBlock = newBlock("json.union.end");
    std::vector<std::pair<llvm::Value *, llvm::BasicBlock *>> incoming;
    // See tz_json_typeof: 2 a boolean, 3 a number, 4 a string, else an array.
    for (UnionSlot slot : to->slots()) {
      llvm::BasicBlock *block = newBlock("json.union.slot");
      llvm::BasicBlock *next = newBlock("json.union.next");
      int code = slot == UnionSlot::Bool     ? 2
                 : slot == UnionSlot::Double ? 3
                 : slot == UnionSlot::String ? 4
                                             : -1;
      if (code >= 0)
        builder_.CreateCondBr(builder_.CreateICmpEQ(kind, builder_.getInt32(code)), block, next);
      else
        builder_.CreateBr(block);
      switchTo(block);
      llvm::Value *part = emitJsonConversion(value, from, to->slotPart(slot));
      incoming.push_back({makeTagged(to, slot, part), builder_.GetInsertBlock()});
      builder_.CreateBr(endBlock);
      switchTo(next);
    }
    builder_.CreateUnreachable();
    switchTo(endBlock);
    llvm::PHINode *phi = builder_.CreatePHI(lower(to), unsigned(incoming.size()));
    for (auto [v, block] : incoming)
      phi->addIncoming(v, block);
    return phi;
  }

  if (to->isTagged()) {
    if (from->isTagged()) {
      // The same tags, where the parts lie alike; a part in a slot that `to`
      // lacks converts, unless a check ruled it out.
      std::vector<UnionSlot> possible;
      bool same = true;
      for (UnionSlot slot : from->slots()) {
        UnionSlot target = targetSlot(slot, to);
        if (target == UnionSlot::None)
          continue;
        possible.push_back(slot);
        same &= target == slot && lower(to->slotPart(slot)) == lower(from->slotPart(slot));
      }
      if (same) {
        llvm::Value *result = llvm::Constant::getNullValue(lower(to));
        unsigned words = cast<llvm::StructType>(lower(to))->getNumElements();
        unsigned have = cast<llvm::StructType>(value->getType())->getNumElements();
        for (unsigned i = 0; i < words && i < have; ++i)
          result = builder_.CreateInsertValue(result, builder_.CreateExtractValue(value, i), i);
        return result;
      }
      return forEachSlot(
          value, possible,
          [&](UnionSlot slot) {
            UnionSlot target = targetSlot(slot, to);
            llvm::Value *part = emitConversion(taggedPart(value, from, slot), from->slotPart(slot),
                                               to->slotPart(target));
            return makeTagged(to, target, part);
          },
          lower(to), llvm::Constant::getNullValue(lower(to)));
    }
    // A member, or a union whose members take one slot.
    UnionSlot slot = targetSlot(unionSlot(from), to);
    assert(slot != UnionSlot::None && "Sema converts a member into its union");
    return makeTagged(to, slot, emitConversion(value, from, to->slotPart(slot)));
  }
  // Narrowed: a member of the tagged union, or a part of it.
  if (from->isTagged()) {
    UnionSlot slot = unionSlot(to);
    return emitConversion(taggedPart(value, from, slot), from->slotPart(slot), to);
  }
  // Atoms are atoms, objects references, whatever the union.
  return value;
}

bool CodeGen::narrowsUnion(const Type *from, const Type *to) {
  from = from->nonOptional();
  if (!from->isUnion() || to->isOptional() || to->isNever())
    return false;
  llvm::ArrayRef<const Type *> members = from->members();
  if (to->isUnion())
    return llvm::all_of(to->members(),
                        [&](const Type *m) { return llvm::is_contained(members, m); });
  return llvm::is_contained(members, to);
}

//===----------------------------------------------------------------------===//
// Equality, typeof, truth
//===----------------------------------------------------------------------===//

llvm::Value *CodeGen::emitUnionEqual(llvm::Value *a, llvm::Value *b, const Type *type) {
  // Two values of a tagged union are equal when they hold the same member
  // and those are equal; both undefined are equal too.
  llvm::Value *sameTag = builder_.CreateICmpEQ(unionTag(a), unionTag(b));
  llvm::BasicBlock *before = builder_.GetInsertBlock();
  llvm::BasicBlock *compareBlock = newBlock("union.compare");
  llvm::BasicBlock *endBlock = newBlock("union.equal");
  builder_.CreateCondBr(sameTag, compareBlock, endBlock);
  switchTo(compareBlock);
  llvm::Value *equal = forEachSlot(
      a, type->slots(),
      [&](UnionSlot slot) {
        return emitValuesEqual(taggedPart(a, type, slot), taggedPart(b, type, slot),
                               type->slotPart(slot));
      },
      builder_.getInt1Ty(), builder_.getTrue());
  llvm::BasicBlock *compareEnd = builder_.GetInsertBlock();
  builder_.CreateBr(endBlock);
  switchTo(endBlock);
  llvm::PHINode *phi = builder_.CreatePHI(builder_.getInt1Ty(), 2);
  phi->addIncoming(builder_.getFalse(), before);
  phi->addIncoming(equal, compareEnd);
  return phi;
}

llvm::Value *CodeGen::emitUnionTypeOf(llvm::Value *value, const Type *type) {
  // A select over the tags: each slot has one name.
  llvm::Value *tag = unionTag(value);
  llvm::Value *result = stringLiteral(u"undefined");
  for (UnionSlot slot : type->slots()) {
    llvm::StringRef name = typeOfName(type->slotPart(slot));
    result = builder_.CreateSelect(builder_.CreateICmpEQ(tag, builder_.getInt32(unsigned(slot))),
                                   stringLiteral(std::u16string(name.begin(), name.end())), result);
  }
  return result;
}

llvm::Value *CodeGen::emitUnionTruth(llvm::Value *value, const Type *type) {
  if (isAtomType(type))
    return emitTruth(atomString(value), Type::string());
  if (const Type *stored = storedType(type); stored != type)
    return emitTruth(value, stored);
  if (isObjectUnion(type))
    return builder_.getTrue();
  // Tagged: a string or a boolean as itself, an object always, undefined never.
  return forEachSlot(
      value, type->slots(),
      [&](UnionSlot slot) -> llvm::Value * {
        const Type *part = type->slotPart(slot);
        if (part->isString() || part->isBool() || isAtomType(part) || part->isLiteral())
          return emitTruth(taggedPart(value, type, slot), part);
        return builder_.getTrue();
      },
      builder_.getInt1Ty(), builder_.getFalse());
}

//===----------------------------------------------------------------------===//
// Objects of a union
//===----------------------------------------------------------------------===//

uint32_t CodeGen::plainTypeId(const Type *type) {
  // Every Promise has one number: a union holds one Promise type, and a
  // Promise<never> stands for any.
  if (type->isPromise())
    return kPlainIdBase;
  // The runtime makes the objects of binary data, numbered by their kind.
  if (type->isBytes())
    return kBytesIdBase + uint32_t(type->bytes());
  // A type and its twin with read-only fields lie alike: one number.
  std::vector<Field> fields(type->fields().begin(), type->fields().end());
  for (Field &f : fields)
    f.readonly = false;
  const Type *canonical = type;
  if (type->isTuple()) {
    std::vector<const Type *> elements;
    for (const Field &f : fields)
      elements.push_back(f.type);
    canonical = Type::tuple(elements);
  } else if (!type->isClass() && !type->isClassRef() && !type->isIncomplete() &&
             !type->isRegExp() && !type->isMatch() &&
             !type->isDate() && !type->isIntl() && !type->isCrypto() && !type->isDict() &&
             !type->isAnyObject() && type->bases().empty() && type->overloads().empty()) {
    canonical = Type::object(std::move(fields));
  }
  auto [it, inserted] = plainIds_.try_emplace(canonical, 0);
  if (inserted)
    it->second = kPlainIdBase + uint32_t(plainIds_.size());
  return it->second;
}

llvm::Value *CodeGen::objectClassId(llvm::Value *object) {
  llvm::Value *descriptor = builder_.CreateLoad(
      builder_.getPtrTy(), builder_.CreateConstInBoundsGEP1_64(builder_.getInt8Ty(), object, 8),
      "descriptor");
  return builder_.CreateLoad(builder_.getInt32Ty(),
                             builder_.CreateStructGEP(layoutDescTy_, descriptor, kDescriptorClass),
                             "class");
}

llvm::Value *CodeGen::emitIsOneOf(llvm::Value *id, llvm::ArrayRef<const Type *> types) {
  llvm::Value *result = builder_.getFalse();
  auto test = [&](const Type *t) {
    if (const ClassDecl *cls = t->classDecl()) {
      llvm::Value *in =
          cls->lastId <= cls->id
              ? builder_.CreateICmpEQ(id, builder_.getInt32(cls->id))
              : builder_.CreateICmpULE(builder_.CreateSub(id, builder_.getInt32(cls->id)),
                                       builder_.getInt32(cls->lastId - cls->id));
      result = builder_.CreateOr(result, in);
      return;
    }
    // A regex, a match or an object of Intl is an object of the
    // runtime, which has no number.
    uint32_t number =
        t->isRegExp() || t->isMatch() || t->isIntl() || t->isCrypto() ? 0 : plainTypeId(t);
    result = builder_.CreateOr(result, builder_.CreateICmpEQ(id, builder_.getInt32(number)));
    // A Buffer is a Uint8Array.
    if (t->bytes() == Type::Bytes::Uint8)
      result = builder_.CreateOr(
          result, builder_.CreateICmpEQ(id, builder_.getInt32(plainTypeId(
                                                Type::bytesType(Type::Bytes::Buffer)))));
  };
  for (const Type *t : types) {
    test(t);
    // The objects of the types declared to be it: classes that implement an
    // interface, interfaces that extend it.
    for (const Type *sub : subtypes_)
      if (sub != t && sub->isSubtypeOf(t) && !(sub->classDecl() && t->classDecl()))
        test(sub);
  }
  return result;
}

llvm::Value *CodeGen::unionFieldAddress(llvm::Value *object, const Type *type,
                                        llvm::StringRef name) {
  llvm::ArrayRef<const Type *> members = type->members();
  const llvm::DataLayout &data = module_->getDataLayout();
  auto offsetIn = [&](const Type *m) -> int64_t {
    if (dispatchedMember(m, name))
      return -1;
    const ObjectLayout &layout = objectLayout(m);
    return int64_t(
        data.getStructLayout(layout.type)->getElementOffset(layout.elements[fieldIndex(m, name)]));
  };
  int64_t offset = offsetIn(members.front());
  if (offset >= 0 && llvm::all_of(members, [&](const Type *m) { return offsetIn(m) == offset; }))
    return builder_.CreateConstInBoundsGEP1_64(builder_.getInt8Ty(), object, uint64_t(offset),
                                               name + ".addr");
  // Each member puts it elsewhere: chosen by the class number of the object.
  llvm::Value *id = objectClassId(object);
  llvm::BasicBlock *endBlock = newBlock("union.field.end");
  std::vector<std::pair<llvm::Value *, llvm::BasicBlock *>> incoming;
  for (size_t i = 0; i < members.size(); ++i) {
    llvm::BasicBlock *next = nullptr;
    if (i + 1 < members.size()) {
      llvm::BasicBlock *block = newBlock("union.field");
      next = newBlock("union.field.next");
      builder_.CreateCondBr(emitIsOneOf(id, members[i]), block, next);
      switchTo(block);
    }
    incoming.push_back({fieldAddress(object, members[i], name), builder_.GetInsertBlock()});
    builder_.CreateBr(endBlock);
    if (next)
      switchTo(next);
  }
  switchTo(endBlock);
  llvm::PHINode *phi = builder_.CreatePHI(builder_.getPtrTy(), unsigned(incoming.size()));
  for (auto [v, block] : incoming)
    phi->addIncoming(v, block);
  return phi;
}

llvm::Value *CodeGen::emitUnionFieldRead(MemberExpr *member) {
  const Type *type = member->object->type;
  Operand object = emitOperand(member->object, /*canBorrow=*/true);
  llvm::ArrayRef<const Type *> members = type->members();
  // A field of one type in every member: one load, from one offset if they
  // agree on it.
  const Type *fieldType = members.front()->field(member->name)->type;
  bool same = llvm::all_of(
      members, [&](const Type *m) { return m->field(member->name)->type == fieldType; });
  llvm::Value *result;
  if (same) {
    llvm::Value *address = unionFieldAddress(object.value, type, member->name);
    result = builder_.CreateLoad(lower(fieldType), address, member->name);
    retainValue(result, fieldType);
    result = emitConversion(result, fieldType, member->type);
  } else {
    // Each member reads its own, chosen by the class number of the object.
    llvm::Value *id = objectClassId(object.value);
    llvm::BasicBlock *endBlock = newBlock("union.field.end");
    std::vector<std::pair<llvm::Value *, llvm::BasicBlock *>> incoming;
    for (size_t i = 0; i < members.size(); ++i) {
      const Type *m = members[i];
      llvm::BasicBlock *next = nullptr;
      if (i + 1 < members.size()) {
        llvm::BasicBlock *read = newBlock("union.field");
        next = newBlock("union.field.next");
        builder_.CreateCondBr(emitIsOneOf(id, m), read, next);
        switchTo(read);
      }
      const Type *own = m->field(member->name)->type;
      llvm::Value *value = builder_.CreateLoad(
          lower(own), fieldAddress(object.value, m, member->name), member->name);
      retainValue(value, own);
      incoming.push_back({emitConversion(value, own, member->type), builder_.GetInsertBlock()});
      builder_.CreateBr(endBlock);
      if (next)
        switchTo(next);
    }
    switchTo(endBlock);
    llvm::PHINode *phi = builder_.CreatePHI(lower(member->type), unsigned(incoming.size()));
    for (auto [v, block] : incoming)
      phi->addIncoming(v, block);
    result = phi;
  }
  dropOperand(object);
  return result;
}

llvm::Value *CodeGen::emitObjectIn(BinaryExpr *bin) {
  // `"radius" in s`: whether the object is of a member with that field.
  emitDiscarded(bin->lhs);
  const Type *type = bin->rhs->type;
  Operand value = emitOperand(bin->rhs, /*canBorrow=*/true);
  std::string name;
  const Expr *key = bin->lhs;
  while (auto *conv = dyn_cast<CastExpr>(key))
    key = conv->operand;
  if (auto *text = dyn_cast<StringLitExpr>(key))
    llvm::convertUTF16ToUTF8String(
        llvm::ArrayRef<llvm::UTF16>(reinterpret_cast<const llvm::UTF16 *>(text->value.data()),
                                    text->value.size()),
        name);
  llvm::Value *object = value.value;
  llvm::Value *result;
  const Type *objects = type->nonOptional();
  llvm::Value *present = type->isOptional() ? optionalHas(object, type) : builder_.getTrue();
  if (type->isOptional())
    object = optionalValue(object, type);
  std::vector<const Type *> with;
  if (objects->isTagged()) {
    // An object only in its slot.
    llvm::Value *isObject =
        builder_.CreateICmpEQ(unionTag(object), builder_.getInt32(unsigned(UnionSlot::Object)));
    present = builder_.CreateAnd(present, isObject);
    objects = objects->slotPart(UnionSlot::Object);
    object = builder_.CreateSelect(isObject,
                                   taggedPart(object, value.type->nonOptional(), UnionSlot::Object),
                                   llvm::ConstantPointerNull::get(builder_.getPtrTy()));
  }
  for (const Type *m :
       objects->isUnion() ? objects->members() : llvm::ArrayRef<const Type *>(objects))
    if (m->isObject() && m->field(name))
      with.push_back(m);
  llvm::BasicBlock *before = builder_.GetInsertBlock();
  llvm::BasicBlock *testBlock = newBlock("in.test");
  llvm::BasicBlock *endBlock = newBlock("in.end");
  builder_.CreateCondBr(present, testBlock, endBlock);
  switchTo(testBlock);
  llvm::Value *matches = emitIsOneOf(objectClassId(object), with);
  llvm::BasicBlock *testEnd = builder_.GetInsertBlock();
  builder_.CreateBr(endBlock);
  switchTo(endBlock);
  llvm::PHINode *phi = builder_.CreatePHI(builder_.getInt1Ty(), 2);
  phi->addIncoming(builder_.getFalse(), before);
  phi->addIncoming(matches, testEnd);
  result = phi;
  dropOperand(value);
  return result;
}

llvm::Value *CodeGen::emitUnionMethodCall(CallExpr *call) {
  auto *member = cast<MemberExpr>(call->callee);
  const Type *fnType = member->type;
  bool borrow = staysDuringCall(member->object) && llvm::all_of(call->args, cannotAssign);
  Operand self = emitOperand(member->object, borrow);
  holdOperand(self, llvm::any_of(call->args, [&](Expr *arg) { return exprThrows(arg); }));
  std::vector<llvm::Value *> values = emitHeldValues(call->args);
  takeOperand(self);
  for (size_t i = call->args.size(); i < fnType->params().size(); ++i)
    values.push_back(undefinedValue(fnType->params()[i]));
  // The member the object is of tells which runs; each gets the arguments.
  llvm::Value *id = objectClassId(self.value);
  llvm::BasicBlock *endBlock = newBlock("union.call.end");
  std::vector<std::pair<llvm::Value *, llvm::BasicBlock *>> incoming;
  bool throws = false;
  for (size_t i = 0; i < call->unionMethods.size(); ++i) {
    const CallExpr::UnionMethod &m = call->unionMethods[i];
    llvm::BasicBlock *next = nullptr;
    if (i + 1 < call->unionMethods.size()) {
      llvm::BasicBlock *block = newBlock("union.call");
      next = newBlock("union.call.next");
      builder_.CreateCondBr(emitIsOneOf(id, m.member), block, next);
      switchTo(block);
    }
    llvm::Value *result;
    if (m.method) {
      std::vector<llvm::Value *> args{self.value};
      args.insert(args.end(), values.begin(), values.end());
      result = m.virtualCall
                   ? builder_.CreateCall(closureFnType(functionTypeOf(m.method)),
                                         vtableEntry(self.value, m.method->decl->slot, false), args)
                   : builder_.CreateCall(methodCode(m.method->decl), args);
      throws |= callMayThrow(m.method->decl, m.virtualCall);
    } else {
      // The function in the field, borrowed from the object.
      llvm::Value *fn =
          builder_.CreateLoad(fnTy_, fieldAddress(self.value, m.member, member->name));
      result = callFunctionValue(fn, fnType, values);
      throws |= typeThrows(fnType);
    }
    if (result && !result->getType()->isVoidTy())
      incoming.push_back({result, builder_.GetInsertBlock()});
    builder_.CreateBr(endBlock);
    if (next)
      switchTo(next);
  }
  switchTo(endBlock);
  llvm::Value *result = nullptr;
  if (!incoming.empty()) {
    llvm::PHINode *phi =
        builder_.CreatePHI(incoming.front().first->getType(), unsigned(incoming.size()));
    for (auto [v, block] : incoming)
      phi->addIncoming(v, block);
    result = phi;
  }
  dropOperand(self);
  emitThrowCheckIf(throws && call != returnCall_);
  return result;
}

} // namespace tz
