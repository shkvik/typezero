#pragma once

#include "llvm/ADT/StringRef.h"
#include <optional>

namespace tz {

enum class StreamStatic {
  FROM,
};

struct StreamStaticInfo {
  llvm::StringLiteral name;
  StreamStatic value;
};

class StreamStaticTable {
public:
  static std::optional<StreamStatic> find(llvm::StringRef name);
};

}
