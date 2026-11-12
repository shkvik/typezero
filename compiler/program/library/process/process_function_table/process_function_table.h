#pragma once

#include "llvm/ADT/StringRef.h"

namespace tz {

enum class ProcessFunction {
  ON,
  ON_ASYNC,
  OFF,
  OFF_ASYNC,
  STDIN,
  STDOUT,
};

struct ProcessFunctionInfo {
  llvm::StringLiteral name;
  ProcessFunction value;
};

class ProcessFunctionTable {
public:
  static llvm::StringRef getName(ProcessFunction value);
};

}
