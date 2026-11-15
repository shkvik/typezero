#pragma once

#include "llvm/ADT/StringRef.h"
#include <string>

namespace tz {

enum class ScopeStackContextDeclareErrorCode {
  UNDEFINED_AS_NAME,
  DUPLICATE_DECLARATION,
  DUPLICATE_FUNCTION,
};

struct ScopeStackContextDeclareError {
  ScopeStackContextDeclareErrorCode code;
  std::string message;
};

class ErrorTextDeclareName {
public:
  static std::string undefined();
  static std::string previous();
  static std::string function(llvm::StringRef name);
  static std::string duplicate(llvm::StringRef name);
};

}
