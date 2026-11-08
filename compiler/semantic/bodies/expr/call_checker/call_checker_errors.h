#pragma once

#include "llvm/ADT/StringRef.h"
#include <string>

namespace tz {

enum class CallCheckerCheckValueCallErrorCode {
  TAKES_THIS,
};

struct CallCheckerCheckValueCallError {
  CallCheckerCheckValueCallErrorCode code;
  std::string message;
};

class ErrorTextValueCall {
public:
  static std::string takesThis();
};

enum class CallCheckerNoTypeArgsErrorCode {
  TYPE_ARGUMENTS_GIVEN,
};

struct CallCheckerNoTypeArgsError {
  CallCheckerNoTypeArgsErrorCode code;
  std::string message;
};

class ErrorTextCallTypeArgs {
public:
  static std::string forGeneric();
  static std::string notGeneric(llvm::StringRef name);
};

enum class CallCheckerDirectTypeArgsErrorCode {
  NOT_GENERIC,
};

struct CallCheckerDirectTypeArgsError {
  CallCheckerDirectTypeArgsErrorCode code;
  std::string message;
};

enum class CallCheckerCalleeCallableErrorCode {
  MAY_BE_UNDEFINED,
  ALREADY_REPORTED,
  NOT_A_FUNCTION,
  NOT_CALLABLE,
};

struct CallCheckerCalleeCallableError {
  CallCheckerCalleeCallableErrorCode code;
  std::string message;
};

class ErrorTextCallee {
public:
  static std::string undefined();
  static std::string notFunc(llvm::StringRef name, llvm::StringRef type);
  static std::string uncallable(llvm::StringRef type);
};

enum class CallCheckerSpreadFreeErrorCode {
  SPREAD_ARGUMENT,
};

struct CallCheckerSpreadFreeError {
  CallCheckerSpreadFreeErrorCode code;
  std::string message;
};

class ErrorTextSpread {
public:
  static std::string notRest();
};

class ErrorTextCallMethod {
public:
  static std::string undefined(llvm::StringRef name);
  static std::string setsThis(llvm::StringRef name);
  static std::string noProperty(llvm::StringRef name, llvm::StringRef type);
};

class ErrorTextCallName {
public:
  static std::string noSupport(llvm::StringRef name, llvm::StringRef why);
  static std::string notFunc(llvm::StringRef name);
};

}
