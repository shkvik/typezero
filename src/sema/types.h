#pragma once

#include "llvm/ADT/ArrayRef.h"
#include "llvm/ADT/StringRef.h"

#include <string>
#include <vector>

namespace tz {

class Type;

// A field of an object type. An optional field (`x?: T`) has the type
// `T | undefined`: a field that is not there and one that holds undefined are
// the same thing, so `x: T | undefined` declares the same field.
struct Field {
  std::string name;
  const Type *type = nullptr;
  bool readonly = false;
};

// The types of the language. Every type exists once (primitives are
// singletons, composite types are interned), so types are compared by pointer.
//
// Object types are exact and structural: the same fields of the same types
// make the same type, whatever it is called (an interface, an alias or a
// literal). An object type that refers to itself, directly or through other
// declared types, is the one exception: it is identified by its declaration.
class Type {
public:
  enum class Kind {
    Error,     // result of an ill-typed expression; silences follow-up errors
    Void,
    Undefined, // the type of `undefined` itself
    Bool,
    Int,       // 32-bit signed integer
    Long,      // 64-bit signed integer
    Double,    // 64-bit float, also spelled `number`
    String,
    Optional,  // `T | undefined`
    Array,     // `T[]`
    Function,  // `(a: int, b?: string) => T`
    Object,    // `{ x: double; y?: string }`
  };

  Kind kind() const { return kind_; }
  bool isError() const { return kind_ == Kind::Error; }
  bool isVoid() const { return kind_ == Kind::Void; }
  bool isUndefined() const { return kind_ == Kind::Undefined; }
  bool isBool() const { return kind_ == Kind::Bool; }
  bool isInt() const { return kind_ == Kind::Int; }
  bool isString() const { return kind_ == Kind::String; }
  bool isInteger() const { return kind_ == Kind::Int || kind_ == Kind::Long; }
  bool isFloat() const { return kind_ == Kind::Double; }
  bool isNumeric() const { return isInteger() || isFloat(); }
  bool isOptional() const { return kind_ == Kind::Optional; }
  bool isArray() const { return kind_ == Kind::Array; }
  bool isFunction() const { return kind_ == Kind::Function; }
  bool isObject() const { return kind_ == Kind::Object; }

  // The value type of `T | undefined`, the element type of `T[]`, or the
  // return type of a function.
  const Type *inner() const { return inner_; }
  // Function types: the types callers pass (`T | undefined` for an optional
  // parameter), how many arguments a call must give, and whether the last
  // parameter is `...rest: T[]`.
  llvm::ArrayRef<const Type *> params() const { return params_; }
  size_t minArgs() const { return minArgs_; }
  bool hasRest() const { return hasRest_; }
  const Type *ret() const { return inner_; }
  // `T` for `T | undefined`, the type itself otherwise.
  const Type *nonOptional() const { return isOptional() ? inner_ : this; }
  // Object types: the fields in the order of the declaration (or literal)
  // that first made the type.
  llvm::ArrayRef<Field> fields() const { return fields_; }
  const Field *field(llvm::StringRef name) const;
  // A recursive object type while its declaration is being resolved: its
  // fields are not known yet.
  bool isIncomplete() const { return incomplete_; }
  // Whether values hold references that are counted: strings, arrays,
  // functions, objects and optionals of those.
  bool isManaged() const;
  // Whether `new Array<T>(n)` can fill elements of this type with a default:
  // zero, false, the empty string or undefined.
  bool hasZeroValue() const;

  // Width in bits of a numeric type.
  unsigned bitWidth() const;
  llvm::StringRef name() const { return name_; }

  static const Type *error();
  static const Type *voidTy();
  static const Type *undefinedTy();
  static const Type *boolTy();
  static const Type *intTy();
  static const Type *longTy();
  static const Type *doubleTy();
  static const Type *string();
  // `type | undefined`. Optional and undefined types stay as they are.
  static const Type *optional(const Type *type);
  static const Type *array(const Type *element);
  static const Type *function(llvm::ArrayRef<const Type *> params, size_t minArgs, bool hasRest,
                              const Type *ret);
  // The object type with these fields. `name` names it in messages if it is
  // new; otherwise it is spelled out, `{ x: int; y: int }`.
  static const Type *object(std::vector<Field> fields, llvm::StringRef name = {});
  // A recursive object type, while its declaration `name` is resolved: the
  // fields that mention it get this type, and defineFields completes it.
  static Type *declareObject(llvm::StringRef name);
  void defineFields(std::vector<Field> fields);

  // The type a plain name in a type annotation refers to, or null.
  static const Type *fromName(llvm::StringRef name);

private:
  Type(Kind kind, std::string name, const Type *inner = nullptr)
      : kind_(kind), inner_(inner), name_(std::move(name)) {}

  Kind kind_;
  const Type *inner_;
  std::string name_;
  std::vector<const Type *> params_;
  size_t minArgs_ = 0;
  bool hasRest_ = false;
  std::vector<Field> fields_;
  bool incomplete_ = false;
};

// `{ x: int; y?: string }`: an object type spelled out.
std::string objectTypeSpelling(llvm::ArrayRef<Field> fields);

// What `typeof` gives for a value of `type` ("number", "string"...). For
// `T | undefined` that holds a value, it is the name for T.
const char *typeOfName(const Type *type);

} // namespace tz
