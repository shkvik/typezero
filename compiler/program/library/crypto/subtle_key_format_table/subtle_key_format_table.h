#pragma once

#include "llvm/ADT/StringRef.h"
#include <optional>

namespace tz {

enum class SubtleKeyFormat {
  RAW,
  PKCS8,
  SPKI,
  JWK,
};

struct SubtleKeyFormatInfo {
  llvm::StringLiteral name;
  SubtleKeyFormat value;
};

class SubtleKeyFormatTable {
public:
  static std::optional<SubtleKeyFormat> find(llvm::StringRef name);
  static llvm::StringRef getName(SubtleKeyFormat value);
};

}
