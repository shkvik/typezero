#pragma once

#include "llvm/ADT/StringRef.h"
#include <string>

namespace tz {

enum class WeakRefCheckerTargetWrittenErrorCode {
  NO_TARGET_TYPE,
};

struct WeakRefCheckerTargetWrittenError {
  WeakRefCheckerTargetWrittenErrorCode code;
  std::string message;
};

enum class WeakRefCheckerWeakTargetErrorCode {
  NOT_SUPPORTED_YET,
  NOT_AN_OBJECT,
};

struct WeakRefCheckerWeakTargetError {
  WeakRefCheckerWeakTargetErrorCode code;
  std::string message;
};

enum class WeakRefCheckerNewArgsErrorCode {
  NOT_ONE_OBJECT,
};

struct WeakRefCheckerNewArgsError {
  WeakRefCheckerNewArgsErrorCode code;
  std::string message;
};

enum class WeakRefCheckerNewTypeArgsErrorCode {
  SEVERAL_TYPE_ARGUMENTS,
};

struct WeakRefCheckerNewTypeArgsError {
  WeakRefCheckerNewTypeArgsErrorCode code;
  std::string message;
};

enum class WeakRefCheckerMethodNameErrorCode {
  NO_SUCH_METHOD,
};

struct WeakRefCheckerMethodNameError {
  WeakRefCheckerMethodNameErrorCode code;
  std::string message;
};

class ErrorTextWeakRef {
public:
  static std::string noTarget();
  static std::string oneObject();
  static std::string typeArgs();
  static std::string noMethod(llvm::StringRef name, llvm::StringRef type);
};

class ErrorTextWeakTarget {
public:
  static std::string notYet(llvm::StringRef type);
  static std::string notObject(llvm::StringRef type, bool optional);
};

}
