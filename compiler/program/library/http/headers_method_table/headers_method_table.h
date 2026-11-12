#pragma once

#include "llvm/ADT/StringRef.h"
#include <optional>

namespace tz {

enum class HeadersMethod {
  APPEND_HEADER,
  SET_HEADER,
  GET_HEADER,
  HAS_HEADER,
  DELETE_HEADER,
};

struct HeadersMethodInfo {
  llvm::StringLiteral name;
  HeadersMethod value;
};

class HeadersMethodTable {
public:
  static std::optional<HeadersMethod> find(llvm::StringRef name);
};

}
