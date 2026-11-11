#pragma once

#include "llvm/ADT/StringRef.h"
#include <optional>

namespace tz {

enum class StreamHook {
  ON_READ,
  ON_WRITE,
  ON_WRITEV,
  ON_TRANSFORM,
  ON_FLUSH,
  ON_FINAL,
  ON_DESTROY,
  ON_CONSTRUCT,
};

struct StreamHookInfo {
  llvm::StringLiteral name;
  StreamHook value;
};

class StreamHookTable {
public:
  static std::optional<StreamHook> find(llvm::StringRef name);
};

}
