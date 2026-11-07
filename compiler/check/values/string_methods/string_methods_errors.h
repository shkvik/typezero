#pragma once

#include "llvm/ADT/StringRef.h"
#include <string>

namespace tz {

enum class StringMethodsStringMethodErrorCode {
  NO_SUCH_METHOD,
};

struct StringMethodsStringMethodError {
  StringMethodsStringMethodErrorCode code;
  std::string message;
};

class ErrorTextStringMethod {
public:
  static std::string missing(llvm::StringRef name);
};

}
