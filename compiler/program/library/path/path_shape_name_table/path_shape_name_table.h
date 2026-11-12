#pragma once

#include "llvm/ADT/StringRef.h"
#include <optional>

namespace tz {

enum class PathShapeName {
  PARSED_PATH,
  FORMAT_INPUT_PATH_OBJECT,
};

struct PathShapeNameInfo {
  llvm::StringLiteral name;
  PathShapeName value;
};

class PathShapeNameTable {
public:
  static std::optional<PathShapeName> find(llvm::StringRef name);
  static llvm::StringRef getName(PathShapeName value);
};

}
