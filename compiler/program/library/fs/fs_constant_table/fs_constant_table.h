#pragma once

#include "llvm/ADT/StringRef.h"
#include <optional>

namespace tz {

struct FsConstantInfo {
  llvm::StringLiteral name;
  int value;
};

class FsConstantTable {
public:
  static std::optional<int> find(llvm::StringRef name);
};

}
