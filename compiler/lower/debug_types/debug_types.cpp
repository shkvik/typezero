#include "compiler/lower/debug_types/debug_types.h"
#include "compiler/library/result/result_lowerer/result_lowerer.h"
#include "compiler/lower/layouts/object_layouts/object_layouts.h"
#include "compiler/lower/module_builder/module_builder.h"
#include "compiler/lower/types/union_layout/union_layout.h"
#include "llvm/BinaryFormat/Dwarf.h"
#include "llvm/IR/DebugInfoMetadata.h"

namespace tz {

const char *DebugTypes::slotName(UnionSlot slot) {
  switch (slot) {
  case UnionSlot::String:
    return "String";
  case UnionSlot::Atom:
    return "Atom";
  case UnionSlot::Int:
    return "Int";
  case UnionSlot::Double:
    return "Double";
  case UnionSlot::Bool:
    return "Bool";
  case UnionSlot::Decimal:
    return "Decimal";
  case UnionSlot::Object:
    return "Object";
  case UnionSlot::Array:
    return "Array";
  case UnionSlot::Function:
    return "Function";
  case UnionSlot::Iterator:
    return "Iterator";
  case UnionSlot::Json:
    return "Json";
  case UnionSlot::Int8:
    return "Int8";
  case UnionSlot::Int16:
    return "Int16";
  case UnionSlot::Int32:
    return "Int32";
  case UnionSlot::Uint8:
    return "Uint8";
  case UnionSlot::Uint16:
    return "Uint16";
  case UnionSlot::Uint32:
    return "Uint32";
  case UnionSlot::Uint64:
    return "Uint64";
  case UnionSlot::Param:
    return "Param";
  case UnionSlot::Other:
    return "Other";
  case UnionSlot::None:
    break;
  }

  return "None";
}

DebugTypes::DebugTypes(Lowering &owner, llvm::DIBuilder &debugBuilder,
                       llvm::DICompileUnit *compileUnit)
    : lowering(owner),
      builder(debugBuilder),
      unit(compileUnit) {}

bool DebugTypes::usesAtoms() const {
  return this->atomType != nullptr;
}

llvm::DIType *DebugTypes::of(const Type *type) {
  auto found = this->types.find(type);
  if (found != this->types.end()) {
    return found->second;
  }
  llvm::DIType *made = this->make(type);
  this->types[type] = made;

  return made;
}

llvm::DIType *DebugTypes::referenceTo(llvm::DIType *type) {
  return this->builder.createReferenceType(llvm::dwarf::DW_TAG_reference_type, type, 64);
}

llvm::DIType *DebugTypes::basic(llvm::StringRef name, unsigned bits, unsigned encoding) {
  return this->builder.createBasicType(name, bits, encoding);
}

llvm::DIType *DebugTypes::pointer(llvm::DIType *to) {
  return this->builder.createPointerType(to, 64);
}

llvm::DIType *DebugTypes::voidPointer() {
  if (!this->voidPointerType) {
    this->voidPointerType = this->builder.createPointerType(nullptr, 64);
  }

  return this->voidPointerType;
}

std::string DebugTypes::unique(llvm::StringRef name, const void *owner) {
  std::string candidate = name.str();
  for (unsigned n = 2;; ++n) {
    auto [it, inserted] = this->names.try_emplace(candidate, owner);
    if (inserted || it->second == owner) {
      return candidate;
    }
    candidate = name.str() + "_" + std::to_string(n);
  }
}

llvm::DICompositeType *
DebugTypes::structure(llvm::StringRef name, llvm::StructType *layout,
                      llvm::ArrayRef<std::pair<llvm::StringRef, llvm::DIType *>> fields,
                      llvm::DIType *base, llvm::ArrayRef<unsigned> order) {
  const llvm::DataLayout &data = this->lowering.module_->getDataLayout();
  const llvm::StructLayout *offsets = data.getStructLayout(layout);
  auto *temporary = this->builder.createReplaceableCompositeType(llvm::dwarf::DW_TAG_structure_type,
                                                                 name, this->unit, nullptr, 0);
  std::vector<llvm::Metadata *> members;
  if (base) {
    members.push_back(
        this->builder.createInheritance(temporary, base, 0, 0, llvm::DINode::FlagPublic));
  }
  std::vector<unsigned> indices(order.begin(), order.end());
  if (indices.empty()) {
    for (unsigned i = 0; i < fields.size(); ++i) {
      indices.push_back(i);
    }
  }
  for (unsigned i : indices) {
    if (i >= fields.size() || i >= layout->getNumElements() || !fields[i].second ||
        fields[i].first.empty()) {
      continue;
    }
    uint64_t size = data.getTypeAllocSizeInBits(layout->getElementType(i));
    members.push_back(this->builder.createMemberType(temporary, fields[i].first, nullptr, 0, size,
                                                     0, offsets->getElementOffsetInBits(i),
                                                     llvm::DINode::FlagPublic, fields[i].second));
  }
  llvm::DICompositeType *made = this->builder.createStructType(
      this->unit, name, nullptr, 0, offsets->getSizeInBits(), offsets->getAlignment().value() * 8,
      llvm::DINode::FlagZero, nullptr, this->builder.getOrCreateArray(members));

  return this->builder.replaceTemporary(llvm::TempDICompositeType(temporary), made);
}

llvm::DIType *DebugTypes::header() {
  if (!this->headerType) {
    llvm::Type *ptr = this->lowering.builder_.getPtrTy();
    llvm::Type *i64 = this->lowering.builder_.getInt64Ty();
    this->headerType =
        this->structure("tz_object", llvm::StructType::get(this->lowering.ctx_, {i64, ptr}),
                        {{"rc", this->basic("int", 64, llvm::dwarf::DW_ATE_signed)},
                         {"type", this->voidPointer()}});
  }

  return this->headerType;
}

llvm::DIType *DebugTypes::string() {
  if (!this->stringType) {
    llvm::DIType *meta = this->basic("uint64", 64, llvm::dwarf::DW_ATE_unsigned);
    this->stringType = this->structure("string", this->lowering.strTy_,
                                       {{"obj", this->voidPointer()}, {"meta", meta}});
    llvm::Type *i64 = this->lowering.builder_.getInt64Ty();
    llvm::DICompositeType *rope = this->structure(
        "tz_rope",
        llvm::StructType::get(this->lowering.ctx_, {i64, i64, this->lowering.strTy_,
                                                    this->lowering.strTy_, this->lowering.strTy_}),
        {{"rc", this->basic("int", 64, llvm::dwarf::DW_ATE_signed)},
         {"kind", meta},
         {"left", this->stringType},
         {"right", this->stringType},
         {"flat", this->stringType}});
    this->builder.retainType(rope);
  }

  return this->stringType;
}

llvm::DIType *DebugTypes::function() {
  if (!this->functionType) {
    llvm::DISubroutineType *signature =
        this->builder.createSubroutineType(this->builder.getOrCreateTypeArray({nullptr}));
    llvm::DIType *info = this->structure(
        "FunctionInfo", this->lowering.fnInfoTy_,
        {{"code", this->builder.createPointerType(signature, 64)}, {"name", this->string()}});
    this->functionType =
        this->structure("Function", this->lowering.fnTy_,
                        {{"info", this->pointer(info)}, {"env", this->voidPointer()}});
  }

  return this->functionType;
}

llvm::DIType *DebugTypes::atom() {
  if (!this->atomType) {
    llvm::Type *index = this->lowering.builder_.getInt32Ty();
    this->atomType =
        this->structure("atom", llvm::StructType::get(this->lowering.ctx_, llvm::ArrayRef(index)),
                        {{"index", this->basic("int32", 32, llvm::dwarf::DW_ATE_signed)}});
  }

  return this->atomType;
}

llvm::DIType *DebugTypes::opaque(const Type *type) {
  std::string name = this->unique(type->name(), type);
  llvm::Type *ptr = this->lowering.builder_.getPtrTy();
  llvm::Type *i64 = this->lowering.builder_.getInt64Ty();

  return this->structure(name, llvm::StructType::get(this->lowering.ctx_, {i64, ptr}), {});
}

llvm::DIType *DebugTypes::array(const Type *type) {
  llvm::DIType *element = this->of(type->inner());
  if (element->getTag() == llvm::dwarf::DW_TAG_reference_type) {
    element = this->pointer(llvm::cast<llvm::DIDerivedType>(element)->getBaseType());
  }
  llvm::DIType *u32 = this->basic("uint32", 32, llvm::dwarf::DW_ATE_unsigned);

  return this->structure(this->unique("Array<" + type->inner()->name().str() + ">", type),
                         this->lowering.arrayTy_,
                         {{"rc", this->basic("int", 64, llvm::dwarf::DW_ATE_signed)},
                          {"type", this->voidPointer()},
                          {"data", this->pointer(element)},
                          {"length", u32},
                          {"capacity", u32},
                          {"alloc", this->voidPointer()}});
}

llvm::DIType *DebugTypes::optional(const Type *type) {
  const Type *inner = type->inner();
  llvm::DIType *value = this->of(inner);
  if (UnionLayout::zeroIsUndefined(inner)) {
    if (value->getTag() == llvm::dwarf::DW_TAG_reference_type) {
      return this->pointer(llvm::cast<llvm::DIDerivedType>(value)->getBaseType());
    }
    return value;
  }
  auto *layout = llvm::cast<llvm::StructType>(this->lowering.moduleBuilder().lower(type));

  return this->structure(
      this->unique("Optional<" + inner->name().str() + ">", type), layout,
      {{"value", value}, {"has", this->basic("bool", 8, llvm::dwarf::DW_ATE_boolean)}});
}

llvm::DIType *DebugTypes::object(const Type *type) {
  const ClassDecl *cls = type->classDecl();
  std::string name =
      this->unique(cls && !cls->literal() ? cls->name : llvm::StringRef("object"), type);
  auto *temporary = this->builder.createReplaceableCompositeType(llvm::dwarf::DW_TAG_structure_type,
                                                                 name, this->unit, nullptr, 0);
  this->pending.push_back({type, temporary});

  return temporary;
}

void DebugTypes::completeObject(const Type *type, llvm::DICompositeType *temporary) {
  const Lowering::ObjectLayout *layout = this->lowering.interfaces_.count(type)
                                             ? nullptr
                                             : this->lowering.objectLayouts().builtLayout(type);
  llvm::DICompositeType *made;
  if (!layout) {
    llvm::Type *ptr = this->lowering.builder_.getPtrTy();
    llvm::Type *i64 = this->lowering.builder_.getInt64Ty();
    made = this->structure(temporary->getName(),
                           llvm::StructType::get(this->lowering.ctx_, {i64, ptr}),
                           {{"rc", this->basic("int", 64, llvm::dwarf::DW_ATE_signed)},
                            {"type", this->voidPointer()}});
  } else {
    llvm::ArrayRef<Field> fields = type->fields();
    std::vector<std::pair<llvm::StringRef, llvm::DIType *>> named(layout->type->getNumElements());
    for (unsigned i = 0; i < fields.size() && i < layout->elements.size(); ++i) {
      named[layout->elements[i]] = {fields[i].name, this->of(fields[i].type)};
    }
    std::vector<unsigned> order;
    for (unsigned i = 0; i < fields.size() && i < layout->elements.size(); ++i) {
      order.push_back(layout->elements[i]);
    }
    made = this->structure(temporary->getName(), layout->type, named, nullptr, order);
  }
  this->builder.replaceTemporary(llvm::TempDICompositeType(temporary), made);
}

llvm::DIType *DebugTypes::result(const Type *type) {
  Lowering::ResultLayout layout = this->lowering.resultLowerer().resultLayout(type);
  const Type *okType = type->okType();
  const Type *errType = type->errType();
  std::vector<std::pair<llvm::StringRef, llvm::DIType *>> named(4);
  named[layout.value] = {"value", okType->isVoid() ? nullptr : this->of(okType)};
  named[layout.error] = {"error", errType->isVoid() ? nullptr : this->of(errType)};
  named[layout.ok] = {"ok", this->basic("bool", 8, llvm::dwarf::DW_ATE_boolean)};
  named[layout.site] = {"site", this->voidPointer()};
  std::string name = "Result<" + (okType->isVoid() ? std::string("void") : okType->name().str()) +
                     ", " + errType->name().str() + ">";

  return this->structure(this->unique(name, type), layout.type, named);
}

llvm::DIType *DebugTypes::dict(const Type *type) {
  const Type *key = type->keyType();
  const Type *value = type->valueType();
  llvm::Type *i32 = this->lowering.builder_.getInt32Ty();
  llvm::Type *ptr = this->lowering.builder_.getPtrTy();
  llvm::Type *i64 = this->lowering.builder_.getInt64Ty();
  std::vector<llvm::Type *> elements{i32, i32, this->lowering.moduleBuilder().lower(key)};
  llvm::DIType *u32 = this->basic("uint32", 32, llvm::dwarf::DW_ATE_unsigned);
  auto stored = [&](const Type *part) {
    llvm::DIType *made = this->of(part);
    if (made->getTag() == llvm::dwarf::DW_TAG_reference_type) {
      return this->pointer(llvm::cast<llvm::DIDerivedType>(made)->getBaseType());
    }
    return made;
  };
  std::vector<std::pair<llvm::StringRef, llvm::DIType *>> entryFields{
      {"hash", u32}, {"live", u32}, {"key", stored(key)}};
  std::string kind = type->isSet() ? "Set" : type->isMap() ? "Map" : "Record";
  std::string name = kind + "<" + key->name().str();
  if (!type->isSet()) {
    elements.push_back(this->lowering.moduleBuilder().lower(value));
    entryFields.push_back({"value", stored(value)});
    name += ", " + value->name().str();
  }
  name += ">";
  name = this->unique(name, type);
  llvm::DIType *entry = this->structure(
      name + "::Entry", llvm::StructType::get(this->lowering.ctx_, elements), entryFields);
  llvm::StructType *map = llvm::StructType::get(
      this->lowering.ctx_, {i64, ptr, ptr, ptr, i32, i32, i32, i32, i32, i32, ptr});

  return this->structure(name, map,
                         {{"rc", this->basic("int", 64, llvm::dwarf::DW_ATE_signed)},
                          {"layout", this->voidPointer()},
                          {"entries", this->pointer(entry)},
                          {"index", this->voidPointer()},
                          {"count", u32},
                          {"used", u32},
                          {"capacity", u32},
                          {"index_mask", u32},
                          {"iterating", u32},
                          {"flags", u32},
                          {"next_dead", this->voidPointer()}});
}

llvm::DIType *DebugTypes::tagged(const Type *type) {
  auto *layout = llvm::cast<llvm::StructType>(this->lowering.moduleBuilder().lower(type));
  const llvm::DataLayout &data = this->lowering.module_->getDataLayout();
  uint64_t payloadBits = data.getTypeAllocSizeInBits(layout) - 64;
  std::string name = this->unique("Union<" + type->name().str() + ">", type);
  auto *payload = this->builder.createReplaceableCompositeType(
      llvm::dwarf::DW_TAG_union_type, name + "::Payload", this->unit, nullptr, 0);
  std::vector<llvm::Metadata *> members;
  for (UnionSlot slot : type->slots()) {
    const Type *part = type->slotPart(slot);
    if (!part) {
      continue;
    }
    llvm::DIType *partType = this->of(part);
    if (partType->getTag() == llvm::dwarf::DW_TAG_reference_type) {
      partType = this->pointer(llvm::cast<llvm::DIDerivedType>(partType)->getBaseType());
    }
    uint64_t size = data.getTypeAllocSizeInBits(this->lowering.moduleBuilder().lower(part));
    members.push_back(this->builder.createMemberType(payload, DebugTypes::slotName(slot), nullptr,
                                                     0, size, 0, 0, llvm::DINode::FlagPublic,
                                                     partType));
  }
  llvm::DICompositeType *union_ = this->builder.createUnionType(
      this->unit, name + "::Payload", nullptr, 0, payloadBits, 64, llvm::DINode::FlagZero,
      this->builder.getOrCreateArray(members));
  union_ = this->builder.replaceTemporary(llvm::TempDICompositeType(payload), union_);
  llvm::Type *i32 = this->lowering.builder_.getInt32Ty();
  llvm::Type *words = llvm::ArrayType::get(this->lowering.builder_.getInt64Ty(), payloadBits / 64);

  return this->structure(
      name, llvm::StructType::get(this->lowering.ctx_, {i32, words}),
      {{"tag", this->basic("uint32", 32, llvm::dwarf::DW_ATE_unsigned)}, {"payload", union_}});
}

llvm::DIType *DebugTypes::make(const Type *type) {
  switch (type->kind()) {
  case Type::Kind::Void:
  case Type::Kind::Undefined:
  case Type::Kind::Never:
  case Type::Kind::Error:
    return this->basic("undefined", 8, llvm::dwarf::DW_ATE_boolean);
  case Type::Kind::Bool:
    return this->basic("bool", 8, llvm::dwarf::DW_ATE_boolean);
  case Type::Kind::Int:
    return this->basic(type->name(), type->bitWidth(),
                       type->isUnsigned() ? llvm::dwarf::DW_ATE_unsigned
                                          : llvm::dwarf::DW_ATE_signed);
  case Type::Kind::Double:
    return this->basic("double", 64, llvm::dwarf::DW_ATE_float);
  case Type::Kind::Decimal:
    return this->structure("decimal", this->lowering.decTy_,
                           {{"coef", this->basic("int", 64, llvm::dwarf::DW_ATE_signed)},
                            {"meta", this->basic("uint64", 64, llvm::dwarf::DW_ATE_unsigned)}});
  case Type::Kind::String:
    return this->string();
  case Type::Kind::Array:
    return this->referenceTo(this->array(type));
  case Type::Kind::Optional:
    return this->optional(type);
  case Type::Kind::Function:
    return this->function();
  case Type::Kind::Object:
    return this->referenceTo(this->object(type));
  case Type::Kind::Result:
    return this->result(type);
  case Type::Kind::Dict:
    return this->referenceTo(this->dict(type));
  case Type::Kind::Literal:
  case Type::Kind::Union: {
    if (UnionLayout::isAtomType(type)) {
      return this->atom();
    }
    if (UnionLayout::isObjectUnion(type)) {
      return this->pointer(this->header());
    }
    const Type *stored = UnionLayout::storedType(type);
    if (stored != type) {
      return this->of(stored);
    }
    return this->tagged(type);
  }
  case Type::Kind::Param:
  case Type::Kind::Iterator:
  case Type::Kind::Json:
  case Type::Kind::Promise:
  case Type::Kind::Unknown:
  case Type::Kind::ClassRef:
    break;
  }

  return this->pointer(this->opaque(type));
}

void DebugTypes::finish() {
  while (!this->pending.empty()) {
    auto [type, temporary] = this->pending.back();
    this->pending.pop_back();
    this->completeObject(type, temporary);
  }
}

}
