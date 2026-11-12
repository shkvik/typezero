#pragma once

#include "llvm/ADT/StringRef.h"
#include <optional>

namespace tz {

enum class TypeOfName {
  UNDEFINED_TYPE,
  NULL_TYPE,
  BOOLEAN,
  STRING,
  DOUBLE,
  ARRAY,
  OBJECT,
  NUMBER,
  BIGINT,
  SYMBOL,
};

struct TypeOfNameInfo {
  llvm::StringLiteral name;
  TypeOfName value;
};

class TypeOfNameTable {
public:
  static std::optional<TypeOfName> find(llvm::StringRef name);
  static llvm::StringRef getName(TypeOfName value);
};

}
