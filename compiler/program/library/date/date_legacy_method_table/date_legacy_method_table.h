#pragma once

#include "llvm/ADT/StringRef.h"
#include <optional>

namespace tz {

enum class DateLegacyMethod {
  GET_YEAR,
  SET_YEAR,
  TO_GMT_STRING,
};

struct DateLegacyMethodInfo {
  llvm::StringLiteral name;
  DateLegacyMethod value;
};

class DateLegacyMethodTable {
public:
  static std::optional<DateLegacyMethod> find(llvm::StringRef name);
};

}
