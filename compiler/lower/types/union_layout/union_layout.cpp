#include "compiler/lower/types/union_layout/union_layout.h"
#include "compiler/lower/function_frame/function_frame.h"
#include "compiler/lower/layouts/type_descriptors/type_descriptors.h"
#include "compiler/lower/memory/ref_counts/ref_counts.h"
#include "compiler/lower/module_builder/module_builder.h"
#include "compiler/lower/values/string_lowerer/string_lowerer.h"
#include "llvm/IR/MDBuilder.h"

using llvm::cast;
using llvm::dyn_cast;

namespace tz {

UnionLayout::UnionLayout(Lowering &owner)
    : lowering(owner) {}

llvm::Value *UnionLayout::toWord(llvm::IRBuilder<> &b, llvm::Value *v) {
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

llvm::Value *UnionLayout::fromWord(llvm::IRBuilder<> &b, llvm::Value *word, llvm::Type *type) {
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

unsigned UnionLayout::slotWords(UnionSlot slot) {
  switch (slot) {
    case UnionSlot::String:
    case UnionSlot::Decimal:
    case UnionSlot::Function:
      return 2;
    default:
      return 1;
  }
}

bool UnionLayout::isReferenceLike(const Type *type) {
  return type->isObject() || type->isIterator() || type->isJson() || type->isDict() ||
         type->isPromise() || type->isUnknown() || type->isClassRef() ||
         (type->isUnion() && !type->isTagged() && type->slots().front() == UnionSlot::Object);
}

bool UnionLayout::isAtomType(const Type *type) {
  return type->isStringLiteral() ||
         (type->isUnion() && !type->isTagged() && type->slots().front() == UnionSlot::Atom);
}

bool UnionLayout::isObjectUnion(const Type *type) {
  return type->isUnion() && !type->isTagged() && type->slots().front() == UnionSlot::Object;
}

const Type *UnionLayout::storedType(const Type *type) {
  if (type->isOptional()) {
    const Type *inner = UnionLayout::storedType(type->inner());
    return inner == type->inner() ? type : Type::optional(inner);
  }
  if (type->isLiteral() && !type->isStringLiteral()) {
    return type->literalBase();
  }
  if (type->isUnion() && !type->isTagged()) {
    UnionSlot slot = type->slots().front();
    if (slot == UnionSlot::Int) {
      return Type::intTy();
    }
    if (const Type *part = type->slotPart(slot); part && slot >= UnionSlot::Int8 &&
                                                  slot <= UnionSlot::Uint64) {
      return part->isLiteral() ? part->literalBase()
             : part->isUnion() ? part->members().front()->literalBase()
                               : part;
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
  return UnionLayout::isReferenceLike(inner) || UnionLayout::isAtomType(inner) || inner->isTagged();
}

llvm::Type *UnionLayout::lowerUnion(const Type *type) {
  if (UnionLayout::isAtomType(type)) {
    return this->lowering.builder.getInt32Ty();
  }
  if (UnionLayout::isObjectUnion(type)) {
    return this->lowering.builder.getPtrTy();
  }
  const Type *stored = UnionLayout::storedType(type);
  if (stored != type) {
    return this->lowering.moduleBuilder().lower(stored);
  }
  unsigned words = 1;
  for (UnionSlot slot : type->slots()) {
    words = std::max(words, UnionLayout::slotWords(slot));
  }
  llvm::Type *i64 = this->lowering.builder.getInt64Ty();

  return words == 1
             ? llvm::StructType::get(this->lowering.ctx, {this->lowering.builder.getInt32Ty(), i64})
             : llvm::StructType::get(this->lowering.ctx,
                                     {this->lowering.builder.getInt32Ty(), i64, i64});
}

llvm::GlobalVariable *UnionLayout::atomTable() {
  if (this->atomTableVariable) {
    return this->atomTableVariable;
  }
  std::vector<llvm::Constant *> strings{this->lowering.stringLowerer().stringLiteral(u"undefined")};
  for (const Type *literal : Type::stringLiterals()) {
    strings.push_back(this->lowering.stringLowerer().stringLiteral(literal->literalText()));
  }
  auto *arrayType = llvm::ArrayType::get(this->lowering.strTy, strings.size());
  this->atomTableVariable = new llvm::GlobalVariable(
      *this->lowering.module_, arrayType, true, llvm::GlobalValue::PrivateLinkage,
      llvm::ConstantArray::get(arrayType, strings), "tz.atoms");

  return this->atomTableVariable;
}

llvm::Value *UnionLayout::atomString(llvm::Value *atom) {
  auto *constant = dyn_cast<llvm::ConstantInt>(atom);
  if (constant) {
    uint64_t index = constant->getZExtValue();
    llvm::ArrayRef<const Type *> literals = Type::stringLiterals();
    return index == 0 || index > literals.size()
               ? this->lowering.stringLowerer().stringLiteral(u"undefined")
               : this->lowering.stringLowerer().stringLiteral(literals[index - 1]->literalText());
  }
  llvm::GlobalVariable *table = this->atomTable();
  llvm::Value *address = this->lowering.builder.CreateInBoundsGEP(
      table->getValueType(), table,
      {this->lowering.builder.getInt64(0),
       this->lowering.builder.CreateZExt(atom, this->lowering.builder.getInt64Ty())});

  return this->lowering.builder.CreateLoad(this->lowering.strTy, address, "atom.text");
}

llvm::Constant *UnionLayout::atomDescriptor() {
  if (this->atomDescriptorConstant) {
    return this->atomDescriptorConstant;
  }
  auto *descType = llvm::StructType::get(
      this->lowering.ctx, {this->lowering.typeDescTy, this->lowering.builder.getPtrTy()});
  llvm::Constant *base = llvm::ConstantStruct::get(
      this->lowering.typeDescTy,
      {this->lowering.builder.getInt32(15), this->lowering.builder.getInt32(4),
       llvm::ConstantPointerNull::get(this->lowering.builder.getPtrTy())});
  auto *global = new llvm::GlobalVariable(
      *this->lowering.module_, descType, true, llvm::GlobalValue::PrivateLinkage,
      llvm::ConstantStruct::get(descType, {base, this->atomTable()}), "tz.type.atom");
  this->atomDescriptorConstant = global;

  return global;
}

llvm::Constant *UnionLayout::unionDescriptor(const Type *type) {
  if (UnionLayout::isAtomType(type)) {
    return this->atomDescriptor();
  }
  if (UnionLayout::isObjectUnion(type)) {
    return this->lowering.typeDescriptors().typeDescriptor(Type::anyObject());
  }
  const Type *stored = UnionLayout::storedType(type);
  if (stored != type) {
    return this->lowering.typeDescriptors().typeDescriptor(stored);
  }
  llvm::Constant *found = this->lowering.typeDescriptors_.lookup(type);
  if (found) {
    return found;
  }
  constexpr unsigned kSlots = 20;
  auto *slotsType = llvm::ArrayType::get(this->lowering.builder.getPtrTy(), kSlots);
  std::vector<llvm::Constant *> slots(
      kSlots, llvm::ConstantPointerNull::get(this->lowering.builder.getPtrTy()));
  for (UnionSlot slot : type->slots()) {
    slots[unsigned(slot)] = this->lowering.typeDescriptors().typeDescriptor(type->slotPart(slot));
  }
  uint64_t size = this->lowering.module_->getDataLayout().getTypeAllocSize(
      this->lowering.moduleBuilder().lower(type));
  llvm::Constant *base = llvm::ConstantStruct::get(
      this->lowering.typeDescTy,
      {this->lowering.builder.getInt32(14), this->lowering.builder.getInt32(uint32_t(size)),
       llvm::ConstantPointerNull::get(this->lowering.builder.getPtrTy())});
  auto *descType =
      llvm::StructType::get(this->lowering.ctx, {this->lowering.typeDescTy, slotsType});
  auto *global = new llvm::GlobalVariable(
      *this->lowering.module_, descType, true, llvm::GlobalValue::PrivateLinkage,
      llvm::ConstantStruct::get(descType, {base, llvm::ConstantArray::get(slotsType, slots)}),
      "tz.type." + type->name());
  global->setUnnamedAddr(llvm::GlobalValue::UnnamedAddr::Global);
  this->lowering.typeDescriptors_[type] = global;

  return global;
}

llvm::Value *UnionLayout::unionTag(llvm::Value *value) {
  return this->lowering.builder.CreateExtractValue(value, 0, "tag");
}

llvm::Value *UnionLayout::makeTagged(const Type *unionType, UnionSlot slot, llvm::Value *part) {
  auto *type = cast<llvm::StructType>(this->lowering.moduleBuilder().lower(unionType));
  llvm::Value *result = llvm::Constant::getNullValue(type);
  result = this->lowering.builder.CreateInsertValue(
      result, this->lowering.builder.getInt32(unsigned(slot)), 0);
  std::vector<llvm::Value *> words;
  auto *structType = dyn_cast<llvm::StructType>(part->getType());
  if (structType) {
    for (unsigned i = 0; i < structType->getNumElements(); ++i) {
      words.push_back(UnionLayout::toWord(this->lowering.builder,
                                          this->lowering.builder.CreateExtractValue(part, i)));
    }
  } else {
    words.push_back(UnionLayout::toWord(this->lowering.builder, part));
  }
  for (unsigned i = 0; i < words.size(); ++i) {
    result = this->lowering.builder.CreateInsertValue(result, words[i], i + 1);
  }

  return result;
}

llvm::Value *UnionLayout::taggedPart(llvm::Value *value, const Type *unionType, UnionSlot slot) {
  llvm::Type *type = this->lowering.moduleBuilder().lower(unionType->slotPart(slot));
  auto *structType = dyn_cast<llvm::StructType>(type);
  if (structType) {
    llvm::Value *part = llvm::UndefValue::get(type);
    for (unsigned i = 0; i < structType->getNumElements(); ++i) {
      part = this->lowering.builder.CreateInsertValue(
          part,
          UnionLayout::fromWord(this->lowering.builder,
                                this->lowering.builder.CreateExtractValue(value, i + 1),
                                structType->getElementType(i)),
          i);
    }
    return part;
  }

  return UnionLayout::fromWord(this->lowering.builder,
                               this->lowering.builder.CreateExtractValue(value, 1), type);
}

llvm::Value *UnionLayout::forEachSlot(llvm::Value *value, llvm::ArrayRef<UnionSlot> slots,
                                      llvm::function_ref<llvm::Value *(UnionSlot)> each,
                                      llvm::Type *resultType, llvm::Value *otherwise) {
  llvm::BasicBlock *endBlock = this->lowering.functionFrame().newBlock("union.end");
  llvm::BasicBlock *otherBlock = this->lowering.functionFrame().newBlock("union.other");
  llvm::SwitchInst *sw = this->lowering.builder.CreateSwitch(this->unionTag(value), otherBlock,
                                                             unsigned(slots.size()));
  llvm::PHINode *phi = nullptr;
  std::vector<std::pair<llvm::Value *, llvm::BasicBlock *>> incoming;
  for (UnionSlot slot : slots) {
    llvm::BasicBlock *block = this->lowering.functionFrame().newBlock("union.slot");
    sw->addCase(this->lowering.builder.getInt32(unsigned(slot)), block);
    this->lowering.functionFrame().switchTo(block);
    llvm::Value *result = each(slot);
    if (this->lowering.functionFrame().blockIsOpen()) {
      incoming.push_back({result, this->lowering.builder.GetInsertBlock()});
      this->lowering.builder.CreateBr(endBlock);
    }
  }
  this->lowering.functionFrame().switchTo(otherBlock);
  if (otherwise) {
    incoming.push_back({otherwise, otherBlock});
  }
  this->lowering.builder.CreateBr(endBlock);
  this->lowering.functionFrame().switchTo(endBlock);
  if (resultType && !resultType->isVoidTy()) {
    phi = this->lowering.builder.CreatePHI(resultType, unsigned(incoming.size()));
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
  llvm::Function *&helper =
      (retain ? this->lowering.unionRetains : this->lowering.unionReleases)[type];
  if (!helper) {
    auto *fnType = llvm::FunctionType::get(this->lowering.builder.getVoidTy(),
                                           {this->lowering.moduleBuilder().lower(type)}, false);
    helper =
        llvm::Function::Create(fnType, llvm::GlobalValue::InternalLinkage,
                               (retain ? "tz.union.retain." : "tz.union.release.") + type->name(),
                               this->lowering.module_.get());
    helper->addFnAttr(llvm::Attribute::NoUnwind);
    llvm::IRBuilder<>::InsertPointGuard guard(this->lowering.builder);
    llvm::Function *outer = this->lowering.function;
    this->lowering.function = helper;
    this->lowering.builder.SetInsertPoint(
        llvm::BasicBlock::Create(this->lowering.ctx, "entry", helper));
    llvm::Value *arg = helper->getArg(0);
    this->forEachSlot(
        arg, managed,
        [&](UnionSlot slot) -> llvm::Value * {
          llvm::Value *part = this->taggedPart(arg, type, slot);
          if (retain) {
            this->lowering.refCounts().retainValue(part, type->slotPart(slot));
          } else {
            this->lowering.refCounts().releaseValue(part, type->slotPart(slot));
          }

          return nullptr;
        },
        nullptr, nullptr);
    this->lowering.builder.CreateRetVoid();
    this->lowering.function = outer;
  }
  this->lowering.builder.CreateCall(helper, {value});
}

}
