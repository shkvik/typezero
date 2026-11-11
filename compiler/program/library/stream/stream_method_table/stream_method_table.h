#pragma once

#include "llvm/ADT/StringRef.h"
#include <optional>

namespace tz {

enum class StreamMethod {
  PUSH,
  UNSHIFT,
  WRITE,
  END,
  SET_ENCODING,
  PIPE,
};

struct StreamMethodInfo {
  llvm::StringLiteral name;
  StreamMethod value;
};

class StreamMethodTable {
public:
  static std::optional<StreamMethod> find(llvm::StringRef name);
};

}
