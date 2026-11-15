#pragma once

#include "llvm/ADT/StringRef.h"
#include <string>

namespace tz {

enum class LibraryPathRegistryPathValueErrorCode {
  MODULE_AS_VALUE,
};

struct LibraryPathRegistryPathValueError {
  LibraryPathRegistryPathValueErrorCode code;
  std::string message;
};

enum class LibraryPathRegistryPathFunctionErrorCode {
  STRING_CALLED,
  MODULE_CALLED,
};

struct LibraryPathRegistryPathFunctionError {
  LibraryPathRegistryPathFunctionErrorCode code;
  std::string message;
};

class ErrorTextPath {
public:
  static std::string throughFns(llvm::StringRef name, llvm::StringRef module);
  static std::string notCalled(llvm::StringRef name);
  static std::string module(llvm::StringRef name);
  static std::string notYet(llvm::StringRef name, llvm::StringRef module);
};

}
