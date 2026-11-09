#pragma once

#include "llvm/ADT/StringRef.h"
#include <string>

namespace tz {

class ErrorTextLoadChain {
public:
  static std::string pending(llvm::StringRef name, bool started);
};

}
