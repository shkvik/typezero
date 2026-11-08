#pragma once

#include "llvm/ADT/StringRef.h"
#include <string>

namespace tz {

enum class ReadlineCheckerReadlineFunctionErrorCode {
  TYPE_ONLY,
};

struct ReadlineCheckerReadlineFunctionError {
  ReadlineCheckerReadlineFunctionErrorCode code;
  std::string message;
};

enum class ReadlineCheckerStdinInputErrorCode {
  NOT_STDIN,
};

struct ReadlineCheckerStdinInputError {
  ReadlineCheckerStdinInputErrorCode code;
  std::string message;
};

class ErrorTextReadline {
public:
  static std::string typeOnly(llvm::StringRef fn, bool promises);
  static std::string option(llvm::StringRef name);
  static std::string noInput();
  static std::string notStdin();
  static std::string output();
};

}
