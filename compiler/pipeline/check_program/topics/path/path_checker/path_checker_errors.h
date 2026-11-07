#pragma once

#include "llvm/ADT/StringRef.h"
#include <string>

namespace tz {

enum class PathCheckerPathValueErrorCode {
  MODULE_AS_VALUE,
};

struct PathCheckerPathValueError {
  PathCheckerPathValueErrorCode code;
  std::string message;
};

enum class PathCheckerPathFunctionErrorCode {
  STRING_CALLED,
  MODULE_CALLED,
};

struct PathCheckerPathFunctionError {
  PathCheckerPathFunctionErrorCode code;
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
