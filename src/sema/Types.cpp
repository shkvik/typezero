#include "sema/Types.h"

#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/StringSwitch.h"
#include "llvm/Support/ErrorHandling.h"

#include <memory>

namespace tz {

bool Type::isManaged() const {
  switch (kind_) {
  case Kind::String:
  case Kind::Array:
    return true;
  case Kind::Optional:
    return inner_->isManaged();
  default:
    return false;
  }
}

bool Type::hasZeroValue() const {
  return isNumeric() || isBool() || isString() || isOptional();
}

unsigned Type::bitWidth() const {
  switch (kind_) {
  case Kind::Int: return 32;
  case Kind::Long:
  case Kind::Double: return 64;
  default: llvm_unreachable("bitWidth() of a non-numeric type");
  }
}

const Type *Type::error() { static const Type t(Kind::Error, "<error>"); return &t; }
const Type *Type::voidTy() { static const Type t(Kind::Void, "void"); return &t; }
const Type *Type::undefinedTy() { static const Type t(Kind::Undefined, "undefined"); return &t; }
const Type *Type::boolTy() { static const Type t(Kind::Bool, "boolean"); return &t; }
const Type *Type::intTy() { static const Type t(Kind::Int, "int"); return &t; }
const Type *Type::longTy() { static const Type t(Kind::Long, "long"); return &t; }
const Type *Type::doubleTy() { static const Type t(Kind::Double, "double"); return &t; }
const Type *Type::string() { static const Type t(Kind::String, "string"); return &t; }

namespace {

// Composite types, created on first use and kept for the whole compilation.
struct Interned {
  llvm::DenseMap<const Type *, std::unique_ptr<Type>> optionals, arrays;
};

Interned &interned() {
  static Interned instance;
  return instance;
}

} // namespace

const Type *Type::optional(const Type *type) {
  if (type->isError() || type->isUndefined() || type->isOptional())
    return type;
  std::unique_ptr<Type> &slot = interned().optionals[type];
  if (!slot)
    slot.reset(new Type(Kind::Optional, type->name().str() + " | undefined", type));
  return slot.get();
}

const Type *Type::array(const Type *element) {
  if (element->isError())
    return element;
  std::unique_ptr<Type> &slot = interned().arrays[element];
  if (!slot) {
    std::string name = element->isOptional() ? "(" + element->name().str() + ")[]"
                                             : element->name().str() + "[]";
    slot.reset(new Type(Kind::Array, std::move(name), element));
  }
  return slot.get();
}

const Type *Type::fromName(llvm::StringRef name) {
  return llvm::StringSwitch<const Type *>(name)
      .Case("void", voidTy())
      .Case("undefined", undefinedTy())
      .Case("boolean", boolTy())
      .Case("int", intTy())
      .Case("long", longTy())
      .Cases({"double", "number"}, doubleTy())
      .Case("string", string())
      .Default(nullptr);
}

} // namespace tz
