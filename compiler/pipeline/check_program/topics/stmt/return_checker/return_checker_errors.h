#pragma once

#include "llvm/ADT/StringRef.h"
#include <string>

namespace tz {

enum class ReturnCheckerEnclosingFunctionErrorCode {
  OUTSIDE_FUNCTION,
};

struct ReturnCheckerEnclosingFunctionError {
  ReturnCheckerEnclosingFunctionErrorCode code;
  std::string message;
};

class ErrorTextReturnPlace {
public:
  static std::string outside();
};

enum class ReturnCheckerConstructorReturnErrorCode {
  VALUE_RETURNED,
  BEFORE_SUPER,
  FIELDS_UNSET,
};

struct ReturnCheckerConstructorReturnError {
  ReturnCheckerConstructorReturnErrorCode code;
  std::string message;
};

class ErrorTextConstructorReturn {
public:
  static std::string value();
  static std::string early();
};

enum class ReturnCheckerAwaitReturnedErrorCode {
  CONCISE_AWAIT,
  RETURN_AWAIT,
};

struct ReturnCheckerAwaitReturnedError {
  ReturnCheckerAwaitReturnedErrorCode code;
  std::string message;
};

class ErrorTextReturnAwait {
public:
  static std::string concise();
  static std::string returned();
};

enum class ReturnCheckerValueRequiredErrorCode {
  MISSING_VALUE,
};

struct ReturnCheckerValueRequiredError {
  ReturnCheckerValueRequiredErrorCode code;
  std::string message;
};

class ErrorTextReturnValue {
public:
  static std::string missing(llvm::StringRef label, llvm::StringRef type);
  static std::string wrong(llvm::StringRef label, llvm::StringRef type, llvm::StringRef given);
  static std::string promise(llvm::StringRef given, llvm::StringRef label, llvm::StringRef type);
  static std::string notVoid(llvm::StringRef label, llvm::StringRef type);
};

enum class ReturnCheckerGeneratorReturnErrorCode {
  WRONG_TYPE,
};

struct ReturnCheckerGeneratorReturnError {
  ReturnCheckerGeneratorReturnErrorCode code;
  std::string message;
};

enum class ReturnCheckerPromiseReturnErrorCode {
  OTHER_PROMISE_TYPE,
};

struct ReturnCheckerPromiseReturnError {
  ReturnCheckerPromiseReturnErrorCode code;
  std::string message;
};

enum class ReturnCheckerVoidReturnErrorCode {
  VALUE_FROM_VOID,
};

struct ReturnCheckerVoidReturnError {
  ReturnCheckerVoidReturnErrorCode code;
  std::string message;
};

}
