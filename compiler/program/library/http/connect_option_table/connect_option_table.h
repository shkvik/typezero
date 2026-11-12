#pragma once

#include "llvm/ADT/StringRef.h"
#include <optional>

namespace tz {

enum class ConnectOption {
  SECURE,
  TRANSPORT,
};

struct ConnectOptionInfo {
  llvm::StringLiteral name;
  ConnectOption value;
};

class ConnectOptionTable {
public:
  static std::optional<ConnectOption> find(llvm::StringRef name);
  static llvm::StringRef getName(ConnectOption value);
};

}
