#pragma once

#include "llvm/ADT/StringRef.h"
#include <optional>

namespace tz {

enum class DateFormatMethod {
  FORMAT,
  FORMAT_TO_PARTS,
  RESOLVED_OPTIONS,
  FORMAT_RANGE,
  FORMAT_RANGE_TO_PARTS,
};

struct DateFormatMethodInfo {
  llvm::StringLiteral name;
  DateFormatMethod value;
};

class DateFormatMethodTable {
public:
  static std::optional<DateFormatMethod> find(llvm::StringRef name);
};

}
