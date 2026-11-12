#pragma once

#include "llvm/ADT/StringRef.h"
#include <optional>

namespace tz {

enum class BitWrapMember {
  FROM,
};

struct BitWrapInfo {
  llvm::StringLiteral name;
  unsigned bits;
};

struct BitWrapMemberInfo {
  llvm::StringLiteral name;
  BitWrapMember member;
};

class BitWrapTable {
public:
  static std::optional<unsigned> findBits(llvm::StringRef name);
  static std::optional<BitWrapMember> findMember(llvm::StringRef name);
};

}
