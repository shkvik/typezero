#pragma once

#include "llvm/ADT/StringRef.h"
#include <optional>

namespace tz {

enum class TimersPromisesFunction {
  SET_TIMEOUT,
  SET_IMMEDIATE,
  SET_INTERVAL,
  SCHEDULER,
};

struct TimersPromisesFunctionInfo {
  llvm::StringLiteral name;
  TimersPromisesFunction value;
};

class TimersPromisesFunctionTable {
public:
  static std::optional<TimersPromisesFunction> find(llvm::StringRef name);
  static llvm::StringRef getName(TimersPromisesFunction value);
};

}
