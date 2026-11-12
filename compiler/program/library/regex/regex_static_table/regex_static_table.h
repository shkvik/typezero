#pragma once

#include "llvm/ADT/StringRef.h"
#include <optional>

namespace tz {

enum class RegexStatic {
  PARSE,
  ESCAPE,
};

struct RegexStaticInfo {
  llvm::StringLiteral name;
  RegexStatic value;
};

class RegexStaticTable {
public:
  static std::optional<RegexStatic> find(llvm::StringRef name);
};

}
