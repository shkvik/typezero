#pragma once

#include "llvm/ADT/StringRef.h"
#include <optional>

namespace tz {

enum class Endianness {
  LITTLE,
  BIG,
};

struct EndiannessInfo {
  llvm::StringLiteral name;
  Endianness value;
};

class EndiannessTable {
public:
  static std::optional<Endianness> find(llvm::StringRef name);
  static llvm::StringRef getName(Endianness value);
};

}
