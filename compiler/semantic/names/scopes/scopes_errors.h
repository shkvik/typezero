#pragma once

#include "llvm/ADT/StringRef.h"
#include <string>

namespace tz {

enum class ScopesDeclareErrorCode {
  UNDEFINED_AS_NAME,
  DUPLICATE_DECLARATION,
  DUPLICATE_FUNCTION,
};

struct ScopesDeclareError {
  ScopesDeclareErrorCode code;
  std::string message;
};

class ErrorTextDeclare {
public:
  static std::string undefined();
  static std::string duplicate(llvm::StringRef name);
  static std::string function(llvm::StringRef name);
  static std::string previous();
};

enum class ScopesCheckVisibleErrorCode {
  DECLARED_IN_OTHER_CASE,
};

struct ScopesCheckVisibleError {
  ScopesCheckVisibleErrorCode code;
  std::string message;
};

class ErrorTextVisible {
public:
  static std::string otherCase(llvm::StringRef name);
};

}
