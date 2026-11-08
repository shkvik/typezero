#pragma once

#include "compiler/program/types/types.h"
#include "llvm/ADT/StringRef.h"
#include <vector>

namespace tz {

class CryptoErrorTypes {
public:
  static bool isCryptoError(const Type *type);
  static std::vector<llvm::StringRef> getTypeNames();
};

}
