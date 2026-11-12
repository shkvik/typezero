#pragma once

#include "llvm/ADT/StringRef.h"
#include <optional>

namespace tz {

enum class FetchInit {
  STATUS,
  STATUS_TEXT,
  HEADERS,
  METHOD,
  BODY,
};

struct FetchInitInfo {
  llvm::StringLiteral name;
  FetchInit value;
};

class FetchInitTable {
public:
  static std::optional<FetchInit> find(llvm::StringRef name);
  static llvm::StringRef getName(FetchInit value);
};

}
