#pragma once

#include "llvm/ADT/StringRef.h"
#include <optional>

namespace tz {

enum class ConsoleMethod {
  ERROR_MESSAGE,
  WARN_MESSAGE,
  LOG_MESSAGE,
  INFO_MESSAGE,
  DEBUG_MESSAGE,
};

struct ConsoleMethodInfo {
  llvm::StringLiteral name;
  ConsoleMethod method;
  bool isToStderr;
};

class ConsoleMethodTable {
public:
  static std::optional<ConsoleMethod> find(llvm::StringRef name);
  static bool isToStderr(ConsoleMethod method);
};

}
