#pragma once

#include "llvm/ADT/StringRef.h"
#include <cstdint>
#include <optional>

namespace tz {

struct CryptoConstantInfo {
  llvm::StringLiteral name;
  int64_t value;
};

class CryptoConstantTable {
public:
  static std::optional<int64_t> find(llvm::StringRef name);
};

}
