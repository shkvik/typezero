#pragma once

#include "llvm/ADT/StringRef.h"

namespace tz {

class ErrorClassTable {
public:
  static bool isErrorClass(llvm::StringRef name);
};

}
