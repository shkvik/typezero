#pragma once

#include "llvm/ADT/StringRef.h"
#include <string>

namespace tz {

enum class LiteralCheckerIntFitsErrorCode {
  DOES_NOT_FIT,
};

struct LiteralCheckerIntFitsError {
  LiteralCheckerIntFitsErrorCode code;
  std::string message;
};

class ErrorTextIntFits {
public:
  static std::string tooBig(llvm::StringRef type);
};

}
