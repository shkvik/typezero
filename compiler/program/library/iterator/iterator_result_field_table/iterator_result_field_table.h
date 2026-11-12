#pragma once

#include "llvm/ADT/StringRef.h"
#include <optional>

namespace tz {

enum class IteratorResultField {
  DONE,
  VALUE,
};

struct IteratorResultFieldInfo {
  llvm::StringLiteral name;
  IteratorResultField value;
};

class IteratorResultFieldTable {
public:
  static std::optional<IteratorResultField> find(llvm::StringRef name);
  static llvm::StringRef getName(IteratorResultField value);
};

}
