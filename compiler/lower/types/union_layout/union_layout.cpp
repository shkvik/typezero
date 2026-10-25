#include "compiler/lower/types/union_layout/union_layout.h"
#include "compiler/lower/module_builder/module_builder.h"
#include "compiler/lower/function_frame/function_frame.h"
#include "compiler/lower/memory/ref_counts/ref_counts.h"
#include "compiler/lower/values/string_lowerer/string_lowerer.h"
#include "compiler/lower/layouts/type_descriptors/type_descriptors.h"
#include "llvm/IR/MDBuilder.h"
#include "llvm/Support/ConvertUTF.h"
#include "llvm/Support/ErrorHandling.h"

using llvm::cast;
using llvm::dyn_cast;
using llvm::isa;
using llvm::BasicBlock;
using llvm::Value;

namespace tz {

namespace {

// The words a value of a slot takes in a tagged union.
unsigned slotWords(UnionSlot slot) {
  switch (slot) {
  case UnionSlot::String:
  case UnionSlot::Decimal:
  case UnionSlot::Function:
    return 2;
  default:
    return 1;
  }
}

// A value held as a pointer, null for undefined: see isReference in
// codegen.cpp.
bool isReferenceLike(const Type *type) {
  return type->isObject() || type->isIterator() || type->isJson() || type->isDict() ||
         type->isPromise() || type->isUnknown() || type->isClassRef() ||
         (type->isUnion() && !type->isTagged() && type->slots().front() == UnionSlot::Object);
}

} // namespace

// A scalar as a word, and back.
static llvm::Value *toWord(llvm::IRBuilder<> &b, llvm::Value *v) {
  llvm::Type *type = v->getType();
  if (type->isPointerTy()) {
    return b.CreatePtrToInt(v, b.getInt64Ty());
  }
  if (type->isDoubleTy()) {
    return b.CreateBitCast(v, b.getInt64Ty());
  }
  if (type->isIntegerTy(64)) {
    return v;
  }

  return b.CreateZExt(v, b.getInt64Ty());
}

static llvm::Value *fromWord(llvm::IRBuilder<> &b, llvm::Value *word, llvm::Type *type) {
  if (type->isPointerTy()) {
    return b.CreateIntToPtr(word, type);
  }
  if (type->isDoubleTy()) {
    return b.CreateBitCast(word, type);
  }
  if (type->isIntegerTy(64)) {
    return word;
  }

  return b.CreateTrunc(word, type);
}

UnionLayout::UnionLayout(CodeGen &codegen) : codegen_(codegen) {}

bool UnionLayout::isAtomType(const Type *type) {
  return type->isStringLiteral() ||
         (type->isUnion() && !type->isTagged() && type->slots().front() == UnionSlot::Atom);
}

bool UnionLayout::isObjectUnion(const Type *type) {
  return type->isUnion() && !type->isTagged() && type->slots().front() == UnionSlot::Object;
}

const Type *UnionLayout::storedType(const Type *type) {
  if (type->isOptional()) {
    const Type *inner = storedType(type->inner());
    return inner == type->inner() ? type : Type::optional(inner);
  }
  // A number or boolean literal, or a union of them, is its base.
  if (type->isLiteral() && !type->isStringLiteral()) {
    return type->literalBase();
  }
  if (type->isUnion() && !type->isTagged()) {
    UnionSlot slot = type->slots().front();
    if (slot == UnionSlot::Int) {
      return Type::intTy();
    }
    if (slot == UnionSlot::Double) {
      return Type::doubleTy();
    }
    if (slot == UnionSlot::Bool) {
      return Type::boolTy();
    }
  }

  return type;
}

bool UnionLayout::zeroIsUndefined(const Type *inner) {
  return isReferenceLike(inner) || isAtomType(inner) || inner->isTagged();
}

llvm::Type *UnionLayout::lowerUnion(const Type *type) {
  if (isAtomType(type)) {
    return codegen_.builder_.getInt32Ty();
  }
  if (isObjectUnion(type)) {
    return codegen_.builder_.getPtrTy();
  }
  if (const Type *stored = storedType(type); stored != type) {
    return codegen_.moduleBuilder().lower(stored);
  }
  unsigned words = 1;
  for (UnionSlot slot : type->slots()) {
    words = std::max(words, slotWords(slot));
  }
  llvm::Type *i64 = codegen_.builder_.getInt64Ty();

  return words == 1 ? llvm::StructType::get(codegen_.ctx_, {codegen_.builder_.getInt32Ty(), i64})
                    : llvm::StructType::get(codegen_.ctx_, {codegen_.builder_.getInt32Ty(), i64, i64});
}

llvm::GlobalVariable *UnionLayout::atomTable() {
  if (atomTable_) {
    return atomTable_;
  }
  // Index 0 is undefined; then the string literals of the program by atom.
  std::vector<llvm::Constant *> strings{codegen_.stringLowerer().stringLiteral(u"undefined")};
  for (const Type *literal : Type::stringLiterals()) {
    strings.push_back(codegen_.stringLowerer().stringLiteral(literal->literalText()));
  }
  auto *arrayType = llvm::ArrayType::get(codegen_.strTy_, strings.size());
  atomTable_ = new llvm::GlobalVariable(*codegen_.module_, arrayType, /*isConstant=*/true,
                                        llvm::GlobalValue::PrivateLinkage,
                                        llvm::ConstantArray::get(arrayType, strings), "tz.atoms");

  return atomTable_;
}

llvm::Value *UnionLayout::atomString(llvm::Value *atom) {
  if (auto *constant = dyn_cast<llvm::ConstantInt>(atom)) {
    uint64_t index = constant->getZExtValue();
    llvm::ArrayRef<const Type *> literals = Type::stringLiterals();
    return index == 0 || index > literals.size()
               ? codegen_.stringLowerer().stringLiteral(u"undefined")
               : codegen_.stringLowerer().stringLiteral(literals[index - 1]->literalText());
  }
  llvm::GlobalVariable *table = atomTable();
  llvm::Value *address = codegen_.builder_.CreateInBoundsGEP(
      table->getValueType(), table,
      {codegen_.builder_.getInt64(0), codegen_.builder_.CreateZExt(atom, codegen_.builder_.getInt64Ty())});

  return codegen_.builder_.CreateLoad(codegen_.strTy_, address, "atom.text");
}

llvm::Constant *UnionLayout::atomDescriptor() {
  if (atomDescriptor_) {
    return atomDescriptor_;
  }
  auto *descType = llvm::StructType::get(codegen_.ctx_, {codegen_.typeDescTy_, codegen_.builder_.getPtrTy()});
  llvm::Constant *base =
      llvm::ConstantStruct::get(codegen_.typeDescTy_, {codegen_.builder_.getInt32(15), codegen_.builder_.getInt32(4),
                                              llvm::ConstantPointerNull::get(codegen_.builder_.getPtrTy())});
  auto *global = new llvm::GlobalVariable(
      *codegen_.module_, descType, /*isConstant=*/true, llvm::GlobalValue::PrivateLinkage,
      llvm::ConstantStruct::get(descType, {base, atomTable()}), "tz.type.atom");
  atomDescriptor_ = global;

  return global;
}

llvm::Constant *UnionLayout::unionDescriptor(const Type *type) {
  if (isAtomType(type)) {
    return atomDescriptor();
  }
  if (isObjectUnion(type)) {
    return codegen_.typeDescriptors().typeDescriptor(Type::anyObject());
  }
  if (const Type *stored = storedType(type); stored != type) {
    return codegen_.typeDescriptors().typeDescriptor(stored);
  }
  if (llvm::Constant *found = codegen_.typeDescriptors_.lookup(type)) {
    return found;
  }
  // { TzType, slots[12] }: TzUnionType in runtime/core/tzrt/tzrt.h.
  constexpr unsigned kSlots = 12;
  auto *slotsType = llvm::ArrayType::get(codegen_.builder_.getPtrTy(), kSlots);
  std::vector<llvm::Constant *> slots(kSlots, llvm::ConstantPointerNull::get(codegen_.builder_.getPtrTy()));
  for (UnionSlot slot : type->slots()) {
    slots[unsigned(slot)] = codegen_.typeDescriptors().typeDescriptor(type->slotPart(slot));
  }
  uint64_t size = codegen_.module_->getDataLayout().getTypeAllocSize(codegen_.moduleBuilder().lower(type));
  llvm::Constant *base = llvm::ConstantStruct::get(
      codegen_.typeDescTy_, {codegen_.builder_.getInt32(14), codegen_.builder_.getInt32(uint32_t(size)),
                    llvm::ConstantPointerNull::get(codegen_.builder_.getPtrTy())});
  auto *descType = llvm::StructType::get(codegen_.ctx_, {codegen_.typeDescTy_, slotsType});
  auto *global = new llvm::GlobalVariable(
      *codegen_.module_, descType, /*isConstant=*/true, llvm::GlobalValue::PrivateLinkage,
      llvm::ConstantStruct::get(descType, {base, llvm::ConstantArray::get(slotsType, slots)}),
      "tz.type." + type->name());
  global->setUnnamedAddr(llvm::GlobalValue::UnnamedAddr::Global);
  codegen_.typeDescriptors_[type] = global;

  return global;
}

llvm::Value *UnionLayout::unionTag(llvm::Value *value) {
  return codegen_.builder_.CreateExtractValue(value, 0, "tag");
}

llvm::Value *UnionLayout::makeTagged(const Type *unionType, UnionSlot slot, llvm::Value *part) {
  auto *type = cast<llvm::StructType>(codegen_.moduleBuilder().lower(unionType));
  llvm::Value *result = llvm::Constant::getNullValue(type);
  result = codegen_.builder_.CreateInsertValue(result, codegen_.builder_.getInt32(unsigned(slot)), 0);
  std::vector<llvm::Value *> words;
  if (auto *structType = dyn_cast<llvm::StructType>(part->getType())) {
    for (unsigned i = 0; i < structType->getNumElements(); ++i) {
      words.push_back(toWord(codegen_.builder_, codegen_.builder_.CreateExtractValue(part, i)));
    }
  } else {
    words.push_back(toWord(codegen_.builder_, part));
  }
  for (unsigned i = 0; i < words.size(); ++i) {
    result = codegen_.builder_.CreateInsertValue(result, words[i], i + 1);
  }

  return result;
}

llvm::Value *UnionLayout::taggedPart(llvm::Value *value, const Type *unionType, UnionSlot slot) {
  llvm::Type *type = codegen_.moduleBuilder().lower(unionType->slotPart(slot));
  if (auto *structType = dyn_cast<llvm::StructType>(type)) {
    llvm::Value *part = llvm::UndefValue::get(type);
    for (unsigned i = 0; i < structType->getNumElements(); ++i) {
      part =
          codegen_.builder_.CreateInsertValue(part,
                                     fromWord(codegen_.builder_, codegen_.builder_.CreateExtractValue(value, i + 1),
                                              structType->getElementType(i)),
                                     i);
    }
    return part;
  }

  return fromWord(codegen_.builder_, codegen_.builder_.CreateExtractValue(value, 1), type);
}

llvm::Value *UnionLayout::forEachSlot(llvm::Value *value, llvm::ArrayRef<UnionSlot> slots,
                                  llvm::function_ref<llvm::Value *(UnionSlot)> each,
                                  llvm::Type *resultType, llvm::Value *otherwise) {
  llvm::BasicBlock *endBlock = codegen_.functionFrame().newBlock("union.end");
  llvm::BasicBlock *otherBlock = codegen_.functionFrame().newBlock("union.other");
  llvm::SwitchInst *sw = codegen_.builder_.CreateSwitch(unionTag(value), otherBlock, unsigned(slots.size()));
  llvm::PHINode *phi = nullptr;
  std::vector<std::pair<llvm::Value *, llvm::BasicBlock *>> incoming;
  for (UnionSlot slot : slots) {
    llvm::BasicBlock *block = codegen_.functionFrame().newBlock("union.slot");
    sw->addCase(codegen_.builder_.getInt32(unsigned(slot)), block);
    codegen_.functionFrame().switchTo(block);
    llvm::Value *result = each(slot);
    if (codegen_.functionFrame().blockIsOpen()) {
      incoming.push_back({result, codegen_.builder_.GetInsertBlock()});
      codegen_.builder_.CreateBr(endBlock);
    }
  }
  codegen_.functionFrame().switchTo(otherBlock);
  if (otherwise) {
    incoming.push_back({otherwise, otherBlock});
  }
  codegen_.builder_.CreateBr(endBlock);
  codegen_.functionFrame().switchTo(endBlock);
  if (resultType && !resultType->isVoidTy()) {
    phi = codegen_.builder_.CreatePHI(resultType, unsigned(incoming.size()));
    for (auto [v, block] : incoming) {
      phi->addIncoming(v ? v : llvm::PoisonValue::get(resultType), block);
    }
  }

  return phi;
}

void UnionLayout::countTagged(llvm::Value *value, const Type *type, bool retain) {
  std::vector<UnionSlot> managed;
  for (UnionSlot slot : type->slots()) {
    if (type->slotPart(slot)->isManaged()) {
      managed.push_back(slot);
    }
  }
  if (managed.empty()) {
    return;
  }
  llvm::Function *&helper = (retain ? codegen_.unionRetains_ : codegen_.unionReleases_)[type];
  if (!helper) {
    // One function for each union, which code calls: the switch is small.
    auto *fnType = llvm::FunctionType::get(codegen_.builder_.getVoidTy(), {codegen_.moduleBuilder().lower(type)}, false);
    helper = llvm::Function::Create(
        fnType, llvm::GlobalValue::InternalLinkage,
        (retain ? "tz.union.retain." : "tz.union.release.") + type->name(), codegen_.module_.get());
    helper->addFnAttr(llvm::Attribute::NoUnwind);
    llvm::IRBuilder<>::InsertPointGuard guard(codegen_.builder_);
    llvm::Function *outer = codegen_.function_;
    codegen_.function_ = helper;
    codegen_.builder_.SetInsertPoint(llvm::BasicBlock::Create(codegen_.ctx_, "entry", helper));
    llvm::Value *arg = helper->getArg(0);
    forEachSlot(
        arg, managed,
        [&](UnionSlot slot) -> llvm::Value * {
          llvm::Value *part = taggedPart(arg, type, slot);
          if (retain) {
            codegen_.refCounts().retainValue(part, type->slotPart(slot));
          } else {
            codegen_.refCounts().releaseValue(part, type->slotPart(slot));
          }

          return nullptr;
        },
        nullptr, nullptr);
    codegen_.builder_.CreateRetVoid();
    codegen_.function_ = outer;
  }
  codegen_.builder_.CreateCall(helper, {value});
}

} // namespace tz
