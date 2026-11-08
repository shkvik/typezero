#pragma once

#include "llvm/ADT/StringRef.h"
#include <string>

namespace tz {

enum class SwitchCheckerValueTypeErrorCode {
  VOID_VALUE,
};

struct SwitchCheckerValueTypeError {
  SwitchCheckerValueTypeErrorCode code;
  std::string message;
};

class ErrorTextValueType {
public:
  static std::string isVoid();
};

enum class SwitchCheckerCaseTypeErrorCode {
  NOT_COMPARABLE,
  RESULTS_COMPARED,
};

struct SwitchCheckerCaseTypeError {
  SwitchCheckerCaseTypeErrorCode code;
  std::string message;
};

class ErrorTextCaseType {
public:
  static std::string mismatch(llvm::StringRef test, llvm::StringRef value);
  static std::string results(llvm::StringRef test, llvm::StringRef value);
};

enum class SwitchCheckerCasesHandledErrorCode {
  CASES_LEFT,
};

struct SwitchCheckerCasesHandledError {
  SwitchCheckerCasesHandledErrorCode code;
  std::string message;
};

class ErrorTextCasesLeft {
public:
  static std::string unhandled(llvm::StringRef names, llvm::StringRef first);
};

}
