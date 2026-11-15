#pragma once

#include "llvm/ADT/StringRef.h"
#include <string>

namespace tz {

enum class DisposeRulesErrorCode {
  WRONG_SHAPE,
  NO_SUPER_CALL,
  THIS_ESCAPES,
  NOT_DISPOSABLE,
  OUTLIVES_BLOCK,
};

struct DisposeRulesError {
  DisposeRulesErrorCode code;
  std::string message;
};

class ErrorTextDispose {
public:
  static std::string shape();
  static std::string noSuper(llvm::StringRef parent);
  static std::string thisLeaves();
  static std::string manual();
  static std::string notUsable(llvm::StringRef type);
  static std::string outlives(llvm::StringRef name);
};

}
