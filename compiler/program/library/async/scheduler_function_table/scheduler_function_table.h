#pragma once

#include "llvm/ADT/StringRef.h"
#include <optional>

namespace tz {

enum class SchedulerFunction {
  WAIT,
  YIELD,
};

struct SchedulerFunctionInfo {
  llvm::StringLiteral name;
  SchedulerFunction value;
};

class SchedulerFunctionTable {
public:
  static std::optional<SchedulerFunction> find(llvm::StringRef name);
};

}
