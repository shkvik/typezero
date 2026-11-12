#pragma once

#include "llvm/ADT/StringRef.h"
#include <optional>
#include <vector>

namespace tz {

enum class SubtleKeyUsage {
  ENCRYPT,
  DECRYPT,
  SIGN,
  VERIFY,
  DERIVE_KEY,
  DERIVE_BITS,
  WRAP_KEY,
  UNWRAP_KEY,
};

struct SubtleKeyUsageInfo {
  llvm::StringLiteral name;
  SubtleKeyUsage value;
};

class SubtleKeyUsageTable {
public:
  static std::optional<SubtleKeyUsage> find(llvm::StringRef name);
  static llvm::StringRef getName(SubtleKeyUsage value);
  static std::vector<llvm::StringRef> getNames();
};

}
