#pragma once

#include "llvm/ADT/StringRef.h"
#include <string>

namespace tz {

enum class OsCheckerOsFunctionErrorCode {
  STRING_CALLED,
};

struct OsCheckerOsFunctionError {
  OsCheckerOsFunctionErrorCode code;
  std::string message;
};

class ErrorTextOs {
public:
  static std::string notCalled(llvm::StringRef name);
  static std::string notYet(llvm::StringRef name);
};

}
