#pragma once

#include "llvm/ADT/StringRef.h"
#include <optional>

namespace tz {

enum class StdStreamMember {
  IS_TTY,
  COLUMNS,
  ROWS,
  WRITE,
};

struct StdStreamMemberInfo {
  llvm::StringLiteral name;
  StdStreamMember value;
};

class StdStreamMemberTable {
public:
  static std::optional<StdStreamMember> find(llvm::StringRef name);
};

}
