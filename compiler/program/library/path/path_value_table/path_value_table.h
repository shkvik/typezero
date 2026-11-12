#pragma once

#include "llvm/ADT/StringRef.h"
#include <optional>

namespace tz {

enum class PathValue {
  SEPARATOR,
  DELIMITER,
  POSIX_MODULE,
  WIN32_MODULE,
};

struct PathValueInfo {
  llvm::StringLiteral name;
  PathValue value;
};

class PathValueTable {
public:
  static std::optional<PathValue> find(llvm::StringRef name);
};

}
