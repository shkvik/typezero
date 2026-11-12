#pragma once

#include "llvm/ADT/StringRef.h"

namespace tz {

class CryptoStreamMethodTable {
public:
  static bool isStreamMethod(llvm::StringRef name);
};

}
