#pragma once

#include "llvm/ADT/StringRef.h"
#include <optional>

namespace tz {

enum class NumberStatic {
  PARSE_INT,
  PARSE_FLOAT,
  IS_INTEGER,
  IS_SAFE_INTEGER,
};

struct NumberStaticInfo {
  llvm::StringLiteral name;
  NumberStatic value;
};

class NumberStaticTable {
public:
  static std::optional<NumberStatic> find(llvm::StringRef name);
};

}
