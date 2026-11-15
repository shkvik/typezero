#pragma once

#include "llvm/ADT/StringRef.h"
#include <string>

namespace tz {

enum class LibraryOsRegistryOsFunctionErrorCode {
  STRING_CALLED,
};

struct LibraryOsRegistryOsFunctionError {
  LibraryOsRegistryOsFunctionErrorCode code;
  std::string message;
};

class ErrorTextOs {
public:
  static std::string notCalled(llvm::StringRef name);
  static std::string notYet(llvm::StringRef name);
};

}
