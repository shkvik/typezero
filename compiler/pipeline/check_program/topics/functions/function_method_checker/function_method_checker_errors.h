#pragma once

#include "llvm/ADT/StringRef.h"
#include <string>

namespace tz {

enum class FunctionMethodCheckerCheckThisCallErrorCode {
  NO_BIND,
  NO_SUCH_PROPERTY,
  NO_THIS_ARGUMENT,
  APPLY_ARGUMENTS,
};

struct FunctionMethodCheckerCheckThisCallError {
  FunctionMethodCheckerCheckThisCallErrorCode code;
  std::string message;
};

class ErrorTextThisCall {
public:
  static std::string noBind();
  static std::string noProperty(llvm::StringRef name);
  static std::string thisFirst(llvm::StringRef name);
  static std::string applyArgs();
};

enum class FunctionMethodCheckerCheckListMethodErrorCode {
  NO_SUCH_METHOD,
  ALREADY_REPORTED,
  WRONG_ARITY,
};

struct FunctionMethodCheckerCheckListMethodError {
  FunctionMethodCheckerCheckListMethodErrorCode code;
  std::string message;
};

class ErrorTextListMethod {
public:
  static std::string noMethod(llvm::StringRef name);
};

}
