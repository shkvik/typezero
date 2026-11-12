#pragma once

#include "llvm/ADT/StringRef.h"
#include <optional>

namespace tz {

enum class OsValue {
  END_OF_LINE,
  DEV_NULL,
};

struct OsValueInfo {
  llvm::StringLiteral name;
  OsValue value;
};

class OsValueTable {
public:
  static std::optional<OsValue> find(llvm::StringRef name);
};

}
