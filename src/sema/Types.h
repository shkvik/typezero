#pragma once

#include "llvm/ADT/StringRef.h"

namespace tz {

// The types of the language. For now only primitives, each a singleton, so
// types are compared by pointer.
class Type {
public:
  enum class Kind {
    Error,  // result of an ill-typed expression; silences follow-up errors
    Void,
    Bool,
    Int,    // 32-bit signed integer
    Long,   // 64-bit signed integer
    Double, // 64-bit float, also spelled `number`
    String, // string literals; only printable so far
  };

  Kind kind() const { return kind_; }
  bool isError() const { return kind_ == Kind::Error; }
  bool isVoid() const { return kind_ == Kind::Void; }
  bool isBool() const { return kind_ == Kind::Bool; }
  bool isString() const { return kind_ == Kind::String; }
  bool isInteger() const { return kind_ == Kind::Int || kind_ == Kind::Long; }
  bool isFloat() const { return kind_ == Kind::Double; }
  bool isNumeric() const { return isInteger() || isFloat(); }

  // Width in bits of a numeric type.
  unsigned bitWidth() const;
  llvm::StringRef name() const;

  static const Type *error();
  static const Type *voidTy();
  static const Type *boolTy();
  static const Type *intTy();
  static const Type *longTy();
  static const Type *doubleTy();
  static const Type *string();

  // The type a name in a type annotation refers to, or null.
  static const Type *fromName(llvm::StringRef name);

private:
  explicit constexpr Type(Kind kind) : kind_(kind) {}

  Kind kind_;
};

} // namespace tz
