#pragma once

#include "llvm/ADT/StringRef.h"
#include <optional>

namespace tz {

enum class BuiltinProperty {
  LENGTH,
  BYTE_LENGTH,
  BYTE_OFFSET,
  BUFFER,
  BYTES_PER_ELEMENT,
  MAX_BYTE_LENGTH,
  RESIZABLE,
  DETACHED,
  ENCODING,
  FATAL,
  IGNORE_BOM,
  PARENT,
  OFFSET,
  POOL_SIZE,
  SIZE,
  RESULT_VALUE,
  RESULT_ERROR,
};

struct BuiltinPropertyInfo {
  llvm::StringLiteral name;
  BuiltinProperty property;
};

class BuiltinPropertyTable {
public:
  static bool isName(llvm::StringRef name, BuiltinProperty property);
  static std::optional<BuiltinProperty> find(llvm::StringRef name);
  static llvm::StringRef getName(BuiltinProperty property);
};

}
