#pragma once

#include "llvm/ADT/StringRef.h"
#include <optional>

namespace tz {

enum class InternalName {
  FIELD_INIT,
};

struct InternalNameInfo {
  llvm::StringLiteral name;
  InternalName value;
};

class InternalNameTable {
public:
  static std::optional<InternalName> find(llvm::StringRef name);
  static llvm::StringRef getName(InternalName value);
};

}
