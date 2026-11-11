#pragma once

#include "llvm/ADT/StringRef.h"
#include <optional>

namespace tz {

enum class StringStatic {
  RAW,
  FROM_CHAR_CODE,
  FROM_CODE_POINT,
};

struct StringStaticInfo {
  llvm::StringLiteral name;
  StringStatic value;
};

class StringStaticTable {
public:
  static std::optional<StringStatic> find(llvm::StringRef name);
};

}
