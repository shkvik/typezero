#pragma once

#include "llvm/ADT/StringRef.h"
#include <string>

namespace tz {

class ErrorTextChildExport {
public:
  static std::string async(llvm::StringRef name, llvm::StringRef path);
  static std::string fork(llvm::StringRef name, llvm::StringRef path);
  static std::string notYet(llvm::StringRef name, llvm::StringRef path);
};

}
