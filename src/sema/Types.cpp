#include "sema/Types.h"

#include "llvm/ADT/StringSwitch.h"
#include "llvm/Support/ErrorHandling.h"

namespace tz {

unsigned Type::bitWidth() const {
  switch (kind_) {
  case Kind::Int: return 32;
  case Kind::Long:
  case Kind::Double: return 64;
  default: llvm_unreachable("bitWidth() of a non-numeric type");
  }
}

llvm::StringRef Type::name() const {
  switch (kind_) {
  case Kind::Error: return "<error>";
  case Kind::Void: return "void";
  case Kind::Bool: return "boolean";
  case Kind::Int: return "int";
  case Kind::Long: return "long";
  case Kind::Double: return "double";
  case Kind::String: return "string";
  }
  llvm_unreachable("unknown type kind");
}

const Type *Type::error() { static const Type t(Kind::Error); return &t; }
const Type *Type::voidTy() { static const Type t(Kind::Void); return &t; }
const Type *Type::boolTy() { static const Type t(Kind::Bool); return &t; }
const Type *Type::intTy() { static const Type t(Kind::Int); return &t; }
const Type *Type::longTy() { static const Type t(Kind::Long); return &t; }
const Type *Type::doubleTy() { static const Type t(Kind::Double); return &t; }
const Type *Type::string() { static const Type t(Kind::String); return &t; }

const Type *Type::fromName(llvm::StringRef name) {
  return llvm::StringSwitch<const Type *>(name)
      .Case("void", voidTy())
      .Case("boolean", boolTy())
      .Case("int", intTy())
      .Case("long", longTy())
      .Cases({"double", "number"}, doubleTy())
      .Case("string", string())
      .Default(nullptr);
}

} // namespace tz
