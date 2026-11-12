#pragma once

#include "compiler/program/types/type/type.h"

namespace tz {

inline constexpr const char *kTypeOfNames[] = {
    "undefined", "null",   "boolean", "int",     "int8",   "int16",  "int32",
    "uint8",     "uint16", "uint32",  "uint64",  "double", "decimal", "string",
    "array",     "object", "function", "class"};

enum class TypeOfNarrowing {
  STRING,
  DOUBLE,
  BOOLEAN,
  ANY_OBJECT,
  DECIMAL,
  INT,
  INT8,
  INT16,
  INT32,
  UINT8,
  UINT16,
  UINT32,
  UINT64,
  UNKNOWN_ARRAY,
};

struct TypeOfNarrowingInfo {
  const char16_t *name;
  TypeOfNarrowing narrowing;
};

class TypeSpelling {
public:
  static bool isTypeOfName(llvm::StringRef name);
  static bool isNumberTypeOfName(llvm::StringRef name);
  static const Type *narrowedByTypeOf(const std::u16string &name);
  static const char *typeOfName(const Type *type);
  static std::vector<llvm::StringRef> typeOfNames(const Type *type);
  static std::string objectTypeSpelling(llvm::ArrayRef<Field> fields);
  static std::string jsonTagField(llvm::ArrayRef<const Type *> objects);

private:
  static const char *integerName(const Type *type);
  static const Type *narrowedType(TypeOfNarrowing narrowing);
};

}
