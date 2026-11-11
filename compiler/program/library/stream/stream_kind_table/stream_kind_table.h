#pragma once

#include "llvm/ADT/StringRef.h"
#include <optional>

namespace tz {

enum class StreamKind {
  READABLE,
  WRITABLE,
  DUPLEX,
  TRANSFORM,
  PASS_THROUGH,
};

struct StreamKindInfo {
  llvm::StringLiteral name;
  StreamKind value;
};

class StreamKindTable {
public:
  static std::optional<StreamKind> find(llvm::StringRef name);
};

}
