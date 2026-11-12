#pragma once

#include "llvm/ADT/StringRef.h"
#include <optional>

namespace tz {

enum class ChildStdio {
  PIPE,
  INHERIT,
  IGNORED,
  IPC,
};

struct ChildStdioInfo {
  llvm::StringLiteral name;
  ChildStdio stdio;
};

class ChildStdioTable {
public:
  static std::optional<ChildStdio> find(llvm::StringRef name);
};

}
