#pragma once

#include "llvm/ADT/StringRef.h"
#include <optional>

namespace tz {

enum class RegexMember {
  TEST,
  EXEC,
  TO_STRING,
  COMPILE,
  LAST_INDEX,
  SOURCE,
  FLAGS,
};

struct RegexMemberInfo {
  llvm::StringLiteral name;
  RegexMember value;
};

class RegexMemberTable {
public:
  static std::optional<RegexMember> find(llvm::StringRef name);
};

}
