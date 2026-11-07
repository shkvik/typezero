#pragma once

#include "llvm/ADT/StringRef.h"
#include <string>

namespace tz {

enum class LiteralCheckerCheckIntLitErrorCode {
  DOES_NOT_FIT,
};

struct LiteralCheckerCheckIntLitError {
  LiteralCheckerCheckIntLitErrorCode code;
  std::string message;
};

class ErrorTextIntLit {
public:
  static std::string tooBig(llvm::StringRef type);
};

}
