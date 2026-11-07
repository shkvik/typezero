#pragma once

#include "llvm/ADT/StringRef.h"
#include <string>

namespace tz {

enum class StaticCheckerFieldTypedErrorCode {
  USED_BEFORE_DECLARATION,
  TYPE_FROM_LATER_VALUE,
};

struct StaticCheckerFieldTypedError {
  StaticCheckerFieldTypedErrorCode code;
  std::string message;
};

class ErrorTextStaticOrder {
public:
  static std::string usedEarly(llvm::StringRef var, llvm::StringRef cls);
  static std::string beforeSet(llvm::StringRef var);
};

enum class StaticCheckerFieldReadyErrorCode {
  USED_BEFORE_DECLARATION,
  READ_BEFORE_SET,
};

struct StaticCheckerFieldReadyError {
  StaticCheckerFieldReadyErrorCode code;
  std::string message;
};

enum class StaticCheckerTypeDeclaredErrorCode {
  TYPE_FROM_LATER_VALUE,
};

struct StaticCheckerTypeDeclaredError {
  StaticCheckerTypeDeclaredErrorCode code;
  std::string message;
};

class ErrorTextStaticLater {
public:
  static std::string typeOf(llvm::StringRef var, llvm::StringRef field);
};

enum class StaticCheckerStaticMethodsErrorCode {
  INSTANCE_MEMBER,
  NO_STATIC_MEMBER,
};

struct StaticCheckerStaticMethodsError {
  StaticCheckerStaticMethodsErrorCode code;
  std::string message;
};

class ErrorTextStaticMember {
public:
  static std::string instance(llvm::StringRef name, llvm::StringRef cls);
  static std::string noMember(llvm::StringRef cls, llvm::StringRef name);
};

enum class StaticCheckerFieldFoundErrorCode {
  STATIC_METHOD,
  NO_STATIC_FIELD,
};

struct StaticCheckerFieldFoundError {
  StaticCheckerFieldFoundErrorCode code;
  std::string message;
};

class ErrorTextStaticField {
public:
  static std::string isMethod(llvm::StringRef name, llvm::StringRef cls);
  static std::string noField(llvm::StringRef cls, llvm::StringRef name);
  static std::string readOnly(llvm::StringRef var);
};

enum class StaticCheckerFieldWritableErrorCode {
  READ_ONLY,
};

struct StaticCheckerFieldWritableError {
  StaticCheckerFieldWritableErrorCode code;
  std::string message;
};

enum class StaticCheckerStaticCallableErrorCode {
  NOT_A_FUNCTION,
  ALREADY_REPORTED,
};

struct StaticCheckerStaticCallableError {
  StaticCheckerStaticCallableErrorCode code;
  std::string message;
};

class ErrorTextStaticCall {
public:
  static std::string notFunc(llvm::StringRef cls, llvm::StringRef name, llvm::StringRef type);
};

enum class StaticCheckerMethodTypeArgsErrorCode {
  NOT_GENERIC,
};

struct StaticCheckerMethodTypeArgsError {
  StaticCheckerMethodTypeArgsErrorCode code;
  std::string message;
};

}
