#pragma once

#include "llvm/ADT/StringRef.h"
#include <optional>

namespace tz {

enum class DateStatic {
  NOW,
  UTC,
  PARSE,
};

struct DateStaticInfo {
  llvm::StringLiteral name;
  DateStatic value;
};

class DateStaticTable {
public:
  static std::optional<DateStatic> find(llvm::StringRef name);
};

}
