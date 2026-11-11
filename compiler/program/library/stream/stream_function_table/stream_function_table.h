#pragma once

#include "llvm/ADT/StringRef.h"
#include <optional>

namespace tz {

enum class StreamFunction {
  PIPELINE,
  FINISHED,
  COMPOSE,
};

struct StreamFunctionInfo {
  llvm::StringLiteral name;
  StreamFunction value;
};

class StreamFunctionTable {
public:
  static std::optional<StreamFunction> find(llvm::StringRef name);
};

}
