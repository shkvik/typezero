#pragma once

#include "llvm/ADT/StringRef.h"
#include <string>

namespace tz {

class ErrorTextTimersExport {
public:
  static std::string none(llvm::StringRef path, llvm::StringRef name);
};

}
