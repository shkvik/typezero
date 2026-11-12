#pragma once

#include "llvm/ADT/StringRef.h"
#include <optional>

namespace tz {

enum class MatchMember {
  INDEX,
  INPUT,
  GROUPS,
  INDICES,
};

struct MatchMemberInfo {
  llvm::StringLiteral name;
  MatchMember value;
};

class MatchMemberTable {
public:
  static std::optional<MatchMember> find(llvm::StringRef name);
  static llvm::StringRef getName(MatchMember value);
};

}
