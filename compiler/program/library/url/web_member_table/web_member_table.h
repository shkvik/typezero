#pragma once

#include "llvm/ADT/StringRef.h"
#include <optional>

namespace tz {

enum class WebMember {
  URL_FIELD,
  HREF,
};

struct WebMemberInfo {
  llvm::StringLiteral name;
  WebMember value;
};

class WebMemberTable {
public:
  static std::optional<WebMember> find(llvm::StringRef name);
  static llvm::StringRef getName(WebMember value);
};

}
