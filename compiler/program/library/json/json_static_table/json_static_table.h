#pragma once

#include "llvm/ADT/StringRef.h"
#include <optional>

namespace tz {

enum class JsonStatic {
  PARSE,
  STRINGIFY,
};

struct JsonStaticInfo {
  llvm::StringLiteral name;
  JsonStatic value;
};

class JsonStaticTable {
public:
  static std::optional<JsonStatic> find(llvm::StringRef name);
};

}
