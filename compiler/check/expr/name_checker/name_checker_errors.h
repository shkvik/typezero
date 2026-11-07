#pragma once

#include "llvm/ADT/StringRef.h"
#include <string>

namespace tz {

enum class NameCheckerTypeArgsAbsentErrorCode {
  NOT_GENERIC,
};

struct NameCheckerTypeArgsAbsentError {
  NameCheckerTypeArgsAbsentErrorCode code;
  std::string message;
};

enum class NameCheckerNotCapturedErrorCode {
  CAPTURED_LOCAL,
};

struct NameCheckerNotCapturedError {
  NameCheckerNotCapturedErrorCode code;
  std::string message;
};

class ErrorTextNameUse {
public:
  static std::string staticUse(llvm::StringRef name);
  static std::string laterUse(llvm::StringRef name);
  static std::string okValue(llvm::StringRef name);
  static std::string resultUse(llvm::StringRef name);
  static std::string typeOnly(llvm::StringRef name, llvm::StringRef make, llvm::StringRef test);
  static std::string noThis();
  static std::string builtin(llvm::StringRef name, llvm::StringRef returns);
  static std::string superUse();
  static std::string arguments();
  static std::string decimal();
  static std::string nullUse();
  static std::string enumUse(llvm::StringRef name);
  static std::string afterCall(llvm::StringRef name, llvm::StringRef early);
  static std::string laterBlock(llvm::StringRef name);
  static std::string beforeDecl(llvm::StringRef name);
  static std::string ownInit(llvm::StringRef name);
  static std::string objectUse(llvm::StringRef name, llvm::StringRef example);
  static std::string notFound(llvm::StringRef name);
};

enum class NameCheckerUnknownNameErrorCode {
  OK_AS_VALUE,
  RESULT_VALUE,
  TYPE_ONLY,
  THIS_OUTSIDE,
  BUILTIN_VALUE,
  SUPER_VALUE,
  ARGUMENTS,
  ALREADY_REPORTED,
  DECIMAL_VALUE,
  NULL_VALUE,
  UNSUPPORTED,
  ENUM_VALUE,
  CAPTURED_LOCAL,
  NOT_LOADED,
  DECLARED_AFTER_CALL,
  DECLARED_LATER_IN_BLOCK,
  USED_BEFORE_DECLARED,
  OWN_INITIALIZER,
  OBJECT_VALUE,
  UNKNOWN_NAME,
};

struct NameCheckerUnknownNameError {
  NameCheckerUnknownNameErrorCode code;
  std::string message;
};

}
