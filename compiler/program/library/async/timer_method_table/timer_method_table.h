#pragma once

#include "llvm/ADT/StringRef.h"
#include <optional>

namespace tz {

enum class TimerMethod {
  REF,
  UNREF,
  HAS_REF,
  REFRESH,
};

struct TimerMethodInfo {
  llvm::StringLiteral name;
  TimerMethod value;
};

class TimerMethodTable {
public:
  static std::optional<TimerMethod> find(llvm::StringRef name);
};

}
