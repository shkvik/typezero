#pragma once

#include "llvm/ADT/StringRef.h"
#include <optional>

namespace tz {

enum class OsShapeName {
  CPU_INFO,
  USER_INFO,
};

struct OsShapeNameInfo {
  llvm::StringLiteral name;
  OsShapeName value;
};

class OsShapeNameTable {
public:
  static std::optional<OsShapeName> find(llvm::StringRef name);
  static llvm::StringRef getName(OsShapeName value);
};

}
