#include "compiler/lower/debug_types/debug_types.h"
#include "compiler/library/result/result_lowerer/result_lowerer.h"
#include "compiler/lower/layouts/object_layouts/object_layouts.h"
#include "compiler/lower/module_builder/module_builder.h"
#include "compiler/lower/types/union_layout/union_layout.h"
#include "llvm/BinaryFormat/Dwarf.h"
#include "llvm/IR/DebugInfoMetadata.h"

namespace tz {

namespace {

const char *slotName(UnionSlot slot) {
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

}

DebugTypes::DebugTypes(Lowering &lowering, llvm::DIBuilder &builder, llvm::DICompileUnit *unit)
    : lowering_(lowering), builder_(builder), unit_(unit) {}

llvm::DIType *DebugTypes::of(const Type *type) {
  if (auto found = types_.find(type); found != types_.end()) {
    return found->second;
  }
  llvm::DIType *made = make(type);
  types_[type] = made;

  return made;
}

llvm::DIType *DebugTypes::referenceTo(llvm::DIType *type) {
  return builder_.createReferenceType(llvm::dwarf::DW_TAG_reference_type, type, 64);
}

llvm::DIType *DebugTypes::basic(llvm::StringRef name, unsigned bits, unsigned encoding) {
  return builder_.createBasicType(name, bits, encoding);
}

llvm::DIType *DebugTypes::pointer(llvm::DIType *to) { return builder_.createPointerType(to, 64); }

llvm::DIType *DebugTypes::voidPointer() {
  if (!voidPointer_) {
    voidPointer_ = builder_.createPointerType(nullptr, 64);
  }

  return voidPointer_;
}

std::string DebugTypes::unique(llvm::StringRef name, const void *owner) {
  std::string candidate = name.str();
  for (unsigned n = 2;; ++n) {
    auto [it, inserted] = names_.try_emplace(candidate, owner);
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
  const llvm::DataLayout &data = lowering_.module_->getDataLayout();
  const llvm::StructLayout *offsets = data.getStructLayout(layout);
  auto *temporary = builder_.createReplaceableCompositeType(llvm::dwarf::DW_TAG_structure_type,
                                                            name, unit_, nullptr, 0);
  std::vector<llvm::Metadata *> members;
  if (base) {
    members.push_back(builder_.createInheritance(temporary, base, 0, 0, llvm::DINode::FlagPublic));
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
    members.push_back(builder_.createMemberType(temporary, fields[i].first, nullptr, 0, size, 0,
                                                offsets->getElementOffsetInBits(i),
                                                llvm::DINode::FlagPublic, fields[i].second));
  }
  llvm::DICompositeType *made = builder_.createStructType(
      unit_, name, nullptr, 0, offsets->getSizeInBits(), offsets->getAlignment().value() * 8,
      llvm::DINode::FlagZero, nullptr, builder_.getOrCreateArray(members));

  return builder_.replaceTemporary(llvm::TempDICompositeType(temporary), made);
}

llvm::DIType *DebugTypes::header() {
  if (!header_) {
    llvm::Type *ptr = lowering_.builder_.getPtrTy(), *i64 = lowering_.builder_.getInt64Ty();
    header_ =
        structure("tz_object", llvm::StructType::get(lowering_.ctx_, {i64, ptr}),
                  {{"rc", basic("int", 64, llvm::dwarf::DW_ATE_signed)}, {"type", voidPointer()}});
  }

  return header_;
}

llvm::DIType *DebugTypes::string() {
  if (!string_) {
    llvm::DIType *meta = basic("uint64", 64, llvm::dwarf::DW_ATE_unsigned);
    string_ = structure("string", lowering_.strTy_, {{"obj", voidPointer()}, {"meta", meta}});
    llvm::Type *i64 = lowering_.builder_.getInt64Ty();
    llvm::DICompositeType *rope =
        structure("tz_rope",
                  llvm::StructType::get(lowering_.ctx_, {i64, i64, lowering_.strTy_,
                                                         lowering_.strTy_, lowering_.strTy_}),
                  {{"rc", basic("int", 64, llvm::dwarf::DW_ATE_signed)},
                   {"kind", meta},
                   {"left", string_},
                   {"right", string_},
                   {"flat", string_}});
    builder_.retainType(rope);
  }

  return string_;
}

llvm::DIType *DebugTypes::function() {
  if (!function_) {
    llvm::DISubroutineType *signature =
        builder_.createSubroutineType(builder_.getOrCreateTypeArray({nullptr}));
    llvm::DIType *info =
        structure("FunctionInfo", lowering_.fnInfoTy_,
                  {{"code", builder_.createPointerType(signature, 64)}, {"name", string()}});
    function_ =
        structure("Function", lowering_.fnTy_, {{"info", pointer(info)}, {"env", voidPointer()}});
  }

  return function_;
}

llvm::DIType *DebugTypes::atom() {
  if (!atom_) {
    llvm::Type *index = lowering_.builder_.getInt32Ty();
    atom_ = structure("atom", llvm::StructType::get(lowering_.ctx_, llvm::ArrayRef(index)),
                      {{"index", basic("int32", 32, llvm::dwarf::DW_ATE_signed)}});
  }

  return atom_;
}

llvm::DIType *DebugTypes::opaque(const Type *type) {
  std::string name = unique(type->name(), type);
  llvm::Type *ptr = lowering_.builder_.getPtrTy(), *i64 = lowering_.builder_.getInt64Ty();

  return structure(name, llvm::StructType::get(lowering_.ctx_, {i64, ptr}), {});
}

llvm::DIType *DebugTypes::array(const Type *type) {
  llvm::DIType *element = of(type->inner());
  if (element->getTag() == llvm::dwarf::DW_TAG_reference_type) {
    element = pointer(llvm::cast<llvm::DIDerivedType>(element)->getBaseType());
  }
  llvm::DIType *u32 = basic("uint32", 32, llvm::dwarf::DW_ATE_unsigned);

  return structure(unique("Array<" + type->inner()->name().str() + ">", type), lowering_.arrayTy_,
                   {{"rc", basic("int", 64, llvm::dwarf::DW_ATE_signed)},
                    {"type", voidPointer()},
                    {"data", pointer(element)},
                    {"length", u32},
                    {"capacity", u32},
                    {"alloc", voidPointer()}});
}

llvm::DIType *DebugTypes::optional(const Type *type) {
  const Type *inner = type->inner();
  llvm::DIType *value = of(inner);
  if (UnionLayout::zeroIsUndefined(inner)) {
    if (value->getTag() == llvm::dwarf::DW_TAG_reference_type) {
      return pointer(llvm::cast<llvm::DIDerivedType>(value)->getBaseType());
    }
    return value;
  }
  auto *layout = llvm::cast<llvm::StructType>(lowering_.moduleBuilder().lower(type));

  return structure(unique("Optional<" + inner->name().str() + ">", type), layout,
                   {{"value", value}, {"has", basic("bool", 8, llvm::dwarf::DW_ATE_boolean)}});
}

llvm::DIType *DebugTypes::object(const Type *type) {
  const ClassDecl *cls = type->classDecl();
  std::string name = unique(cls && !cls->literal() ? cls->name : llvm::StringRef("object"), type);
  auto *temporary = builder_.createReplaceableCompositeType(llvm::dwarf::DW_TAG_structure_type,
                                                            name, unit_, nullptr, 0);
  pending_.push_back({type, temporary});

  return temporary;
}

void DebugTypes::completeObject(const Type *type, llvm::DICompositeType *temporary) {
  const Lowering::ObjectLayout *layout =
      lowering_.interfaces_.count(type) ? nullptr : lowering_.objectLayouts().builtLayout(type);
  llvm::DICompositeType *made;
  if (!layout) {
    llvm::Type *ptr = lowering_.builder_.getPtrTy(), *i64 = lowering_.builder_.getInt64Ty();
    made =
        structure(temporary->getName(), llvm::StructType::get(lowering_.ctx_, {i64, ptr}),
                  {{"rc", basic("int", 64, llvm::dwarf::DW_ATE_signed)}, {"type", voidPointer()}});
  } else {
    llvm::ArrayRef<Field> fields = type->fields();
    std::vector<std::pair<llvm::StringRef, llvm::DIType *>> named(layout->type->getNumElements());
    for (unsigned i = 0; i < fields.size() && i < layout->elements.size(); ++i) {
      named[layout->elements[i]] = {fields[i].name, of(fields[i].type)};
    }
    std::vector<unsigned> order;
    for (unsigned i = 0; i < fields.size() && i < layout->elements.size(); ++i) {
      order.push_back(layout->elements[i]);
    }
    made = structure(temporary->getName(), layout->type, named, nullptr, order);
  }
  builder_.replaceTemporary(llvm::TempDICompositeType(temporary), made);
}

llvm::DIType *DebugTypes::result(const Type *type) {
  Lowering::ResultLayout layout = lowering_.resultLowerer().resultLayout(type);
  const Type *okType = type->okType(), *errType = type->errType();
  std::vector<std::pair<llvm::StringRef, llvm::DIType *>> named(4);
  named[layout.value] = {"value", okType->isVoid() ? nullptr : of(okType)};
  named[layout.error] = {"error", errType->isVoid() ? nullptr : of(errType)};
  named[layout.ok] = {"ok", basic("bool", 8, llvm::dwarf::DW_ATE_boolean)};
  named[layout.site] = {"site", voidPointer()};
  std::string name = "Result<" + (okType->isVoid() ? std::string("void") : okType->name().str()) +
                     ", " + errType->name().str() + ">";

  return structure(unique(name, type), layout.type, named);
}

llvm::DIType *DebugTypes::dict(const Type *type) {
  const Type *key = type->keyType(), *value = type->valueType();
  llvm::Type *i32 = lowering_.builder_.getInt32Ty(), *ptr = lowering_.builder_.getPtrTy();
  llvm::Type *i64 = lowering_.builder_.getInt64Ty();
  std::vector<llvm::Type *> elements{i32, i32, lowering_.moduleBuilder().lower(key)};
  llvm::DIType *u32 = basic("uint32", 32, llvm::dwarf::DW_ATE_unsigned);
  auto stored = [&](const Type *part) {
    llvm::DIType *made = of(part);
    if (made->getTag() == llvm::dwarf::DW_TAG_reference_type) {
      return pointer(llvm::cast<llvm::DIDerivedType>(made)->getBaseType());
    }
    return made;
  };
  std::vector<std::pair<llvm::StringRef, llvm::DIType *>> entryFields{
      {"hash", u32}, {"live", u32}, {"key", stored(key)}};
  std::string kind = type->isSet() ? "Set" : type->isMap() ? "Map" : "Record";
  std::string name = kind + "<" + key->name().str();
  if (!type->isSet()) {
    elements.push_back(lowering_.moduleBuilder().lower(value));
    entryFields.push_back({"value", stored(value)});
    name += ", " + value->name().str();
  }
  name += ">";
  name = unique(name, type);
  llvm::DIType *entry =
      structure(name + "::Entry", llvm::StructType::get(lowering_.ctx_, elements), entryFields);
  llvm::StructType *map = llvm::StructType::get(
      lowering_.ctx_, {i64, ptr, ptr, ptr, i32, i32, i32, i32, i32, i32, ptr});

  return structure(name, map,
                   {{"rc", basic("int", 64, llvm::dwarf::DW_ATE_signed)},
                    {"layout", voidPointer()},
                    {"entries", pointer(entry)},
                    {"index", voidPointer()},
                    {"count", u32},
                    {"used", u32},
                    {"capacity", u32},
                    {"index_mask", u32},
                    {"iterating", u32},
                    {"flags", u32},
                    {"next_dead", voidPointer()}});
}

llvm::DIType *DebugTypes::tagged(const Type *type) {
  auto *layout = llvm::cast<llvm::StructType>(lowering_.moduleBuilder().lower(type));
  const llvm::DataLayout &data = lowering_.module_->getDataLayout();
  uint64_t payloadBits = data.getTypeAllocSizeInBits(layout) - 64;
  std::string name = unique("Union<" + type->name().str() + ">", type);
  auto *payload = builder_.createReplaceableCompositeType(llvm::dwarf::DW_TAG_union_type,
                                                          name + "::Payload", unit_, nullptr, 0);
  std::vector<llvm::Metadata *> members;
  for (UnionSlot slot : type->slots()) {
    const Type *part = type->slotPart(slot);
    if (!part) {
      continue;
    }
    llvm::DIType *partType = of(part);
    if (partType->getTag() == llvm::dwarf::DW_TAG_reference_type) {
      partType = pointer(llvm::cast<llvm::DIDerivedType>(partType)->getBaseType());
    }
    uint64_t size = data.getTypeAllocSizeInBits(lowering_.moduleBuilder().lower(part));
    members.push_back(builder_.createMemberType(payload, slotName(slot), nullptr, 0, size, 0, 0,
                                                llvm::DINode::FlagPublic, partType));
  }
  llvm::DICompositeType *union_ =
      builder_.createUnionType(unit_, name + "::Payload", nullptr, 0, payloadBits, 64,
                               llvm::DINode::FlagZero, builder_.getOrCreateArray(members));
  union_ = builder_.replaceTemporary(llvm::TempDICompositeType(payload), union_);
  llvm::Type *i32 = lowering_.builder_.getInt32Ty();
  llvm::Type *words = llvm::ArrayType::get(lowering_.builder_.getInt64Ty(), payloadBits / 64);

  return structure(
      name, llvm::StructType::get(lowering_.ctx_, {i32, words}),
      {{"tag", basic("uint32", 32, llvm::dwarf::DW_ATE_unsigned)}, {"payload", union_}});
}

llvm::DIType *DebugTypes::make(const Type *type) {
  switch (type->kind()) {
  case Type::Kind::Void:
  case Type::Kind::Undefined:
  case Type::Kind::Never:
  case Type::Kind::Error:
    return basic("undefined", 8, llvm::dwarf::DW_ATE_boolean);
  case Type::Kind::Bool:
    return basic("bool", 8, llvm::dwarf::DW_ATE_boolean);
  case Type::Kind::Int:
    return basic(type->name(), type->bitWidth(),
                 type->isUnsigned() ? llvm::dwarf::DW_ATE_unsigned : llvm::dwarf::DW_ATE_signed);
  case Type::Kind::Double:
    return basic("double", 64, llvm::dwarf::DW_ATE_float);
  case Type::Kind::Decimal:
    return structure("decimal", lowering_.decTy_,
                     {{"coef", basic("int", 64, llvm::dwarf::DW_ATE_signed)},
                      {"meta", basic("uint64", 64, llvm::dwarf::DW_ATE_unsigned)}});
  case Type::Kind::String:
    return string();
  case Type::Kind::Array:
    return referenceTo(array(type));
  case Type::Kind::Optional:
    return optional(type);
  case Type::Kind::Function:
    return function();
  case Type::Kind::Object:
    return referenceTo(object(type));
  case Type::Kind::Result:
    return result(type);
  case Type::Kind::Dict:
    return referenceTo(dict(type));
  case Type::Kind::Literal:
  case Type::Kind::Union:
    if (UnionLayout::isAtomType(type)) {
      return atom();
    }
    if (UnionLayout::isObjectUnion(type)) {
      return pointer(header());
    }
    if (const Type *stored = UnionLayout::storedType(type); stored != type) {
      return of(stored);
    }
    return tagged(type);
  case Type::Kind::Param:
  case Type::Kind::Iterator:
  case Type::Kind::Json:
  case Type::Kind::Promise:
  case Type::Kind::Unknown:
  case Type::Kind::ClassRef:
    break;
  }

  return pointer(opaque(type));
}

void DebugTypes::finish() {
  while (!pending_.empty()) {
    auto [type, temporary] = pending_.back();
    pending_.pop_back();
    completeObject(type, temporary);
  }
}

}
