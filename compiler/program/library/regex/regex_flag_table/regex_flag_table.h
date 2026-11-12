#pragma once

#include "llvm/ADT/StringRef.h"
#include <cstdint>
#include <optional>

namespace tz {

struct RegexFlagInfo {
  llvm::StringLiteral name;
  uint32_t bit;
};

class RegexFlagTable {
public:
  static std::optional<uint32_t> findBit(llvm::StringRef name);
  static bool isLegacyStatic(llvm::StringRef name);
};

}
