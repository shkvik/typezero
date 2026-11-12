#pragma once

#include "llvm/ADT/StringRef.h"
#include <optional>

namespace tz {

enum class WebCryptoMember {
  RANDOM_UUID,
  GET_RANDOM_VALUES,
  SUBTLE,
};

struct WebCryptoMemberInfo {
  llvm::StringLiteral name;
  WebCryptoMember value;
};

class WebCryptoMemberTable {
public:
  static std::optional<WebCryptoMember> find(llvm::StringRef name);
};

}
