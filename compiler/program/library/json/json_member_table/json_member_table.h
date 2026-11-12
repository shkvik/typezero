#pragma once

#include "llvm/ADT/StringRef.h"
#include <optional>

namespace tz {

enum class JsonMember {
  TO_JSON,
};

struct JsonMemberInfo {
  llvm::StringLiteral name;
  JsonMember value;
};

class JsonMemberTable {
public:
  static std::optional<JsonMember> find(llvm::StringRef name);
  static llvm::StringRef getName(JsonMember value);
};

}
