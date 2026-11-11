#pragma once

#include "llvm/ADT/StringRef.h"
#include <optional>

namespace tz {

enum class ChildFunction {
  SPAWN_SYNC,
  EXEC_FILE_SYNC,
  EXEC_SYNC,
};

struct ChildFunctionInfo {
  llvm::StringLiteral name;
  ChildFunction value;
};

class ChildFunctionTable {
public:
  static std::optional<ChildFunction> find(llvm::StringRef name);
};

}
