#pragma once

#include "llvm/ADT/StringRef.h"
#include <string>

namespace tz {

class ErrorTextBufferExport {
public:
  static std::string notYet(llvm::StringRef name, llvm::StringRef path);
};

}
