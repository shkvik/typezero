#pragma once

#include "llvm/ADT/StringRef.h"
#include <optional>

namespace tz {

enum class DesignKey {
  TYPE,
  RETURN_TYPE,
  PARAM_TYPES,
};

struct DesignKeyInfo {
  llvm::StringLiteral name;
  DesignKey value;
};

class DesignKeyTable {
public:
  static llvm::StringRef getName(DesignKey value);
  static std::optional<DesignKey> find(llvm::StringRef name);
};

}
