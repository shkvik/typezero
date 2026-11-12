#pragma once

#include "compiler/program/types/type/type.h"
#include "llvm/ADT/StringRef.h"
#include <optional>

namespace tz {

struct CryptoClassInfo {
  llvm::StringLiteral name;
  Type::Crypto crypto;
};

class CryptoClassTable {
public:
  static std::optional<Type::Crypto> find(llvm::StringRef name);
};

}
