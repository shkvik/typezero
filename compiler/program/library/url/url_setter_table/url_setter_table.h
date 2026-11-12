#pragma once

#include "llvm/ADT/StringRef.h"
#include <optional>

namespace tz {

enum class UrlSetter {
  HREF,
  PROTOCOL,
  HOST,
  HOSTNAME,
  PORT,
};

struct UrlSetterInfo {
  llvm::StringLiteral name;
  UrlSetter value;
};

class UrlSetterTable {
public:
  static std::optional<UrlSetter> find(llvm::StringRef name);
};

}
