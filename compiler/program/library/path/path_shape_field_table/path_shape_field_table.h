#pragma once

#include "llvm/ADT/StringRef.h"
#include <optional>

namespace tz {

enum class PathShapeField {
  ROOT,
  DIRECTORY,
  BASE,
  EXTENSION,
  NAME,
};

struct PathShapeFieldInfo {
  llvm::StringLiteral name;
  PathShapeField value;
};

class PathShapeFieldTable {
public:
  static std::optional<PathShapeField> find(llvm::StringRef name);
  static llvm::StringRef getName(PathShapeField value);
};

}
