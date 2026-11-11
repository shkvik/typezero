#pragma once

#include "compiler/program/library/crypto/crypto_api/crypto_api.h"
#include "compiler/program/types/types.h"
#include "llvm/ADT/StringRef.h"
#include <optional>
#include <vector>

namespace tz {

struct CryptoCallErrorInfo {
  CryptoFn fn;
  llvm::StringLiteral name;
};

class CryptoErrorTypes {
public:
  static bool isCryptoError(const Type *type);
  static std::vector<llvm::StringRef> getTypeNames();
  static std::optional<llvm::StringRef> callErrorName(CryptoFn fn);
};

}
