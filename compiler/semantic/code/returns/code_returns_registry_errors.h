#pragma once

#include "llvm/ADT/StringRef.h"
#include <string>

namespace tz {

enum class CodeReturnsRegistryEnclosingFunctionErrorCode {
  OUTSIDE_FUNCTION,
};

struct CodeReturnsRegistryEnclosingFunctionError {
  CodeReturnsRegistryEnclosingFunctionErrorCode code;
  std::string message;
};

enum class CodeReturnsRegistryConstructorReturnErrorCode {
  VALUE_RETURNED,
  BEFORE_SUPER,
};

struct CodeReturnsRegistryConstructorReturnError {
  CodeReturnsRegistryConstructorReturnErrorCode code;
  std::string message;
};

enum class CodeReturnsRegistryAwaitReturnedErrorCode {
  CONCISE_AWAIT,
  RETURN_AWAIT,
};

struct CodeReturnsRegistryAwaitReturnedError {
  CodeReturnsRegistryAwaitReturnedErrorCode code;
  std::string message;
};

enum class CodeReturnsRegistryValueRequiredErrorCode {
  MISSING_VALUE,
};

struct CodeReturnsRegistryValueRequiredError {
  CodeReturnsRegistryValueRequiredErrorCode code;
  std::string message;
};

enum class CodeReturnsRegistryGeneratorReturnErrorCode {
  WRONG_TYPE,
};

struct CodeReturnsRegistryGeneratorReturnError {
  CodeReturnsRegistryGeneratorReturnErrorCode code;
  std::string message;
};

enum class CodeReturnsRegistryPromiseReturnErrorCode {
  OTHER_PROMISE_TYPE,
};

struct CodeReturnsRegistryPromiseReturnError {
  CodeReturnsRegistryPromiseReturnErrorCode code;
  std::string message;
};

enum class CodeReturnsRegistryVoidReturnErrorCode {
  VALUE_FROM_VOID,
};

struct CodeReturnsRegistryVoidReturnError {
  CodeReturnsRegistryVoidReturnErrorCode code;
  std::string message;
};

class ErrorTextReturnPlace {
public:
  static std::string outside();
};

class ErrorTextConstructorReturn {
public:
  static std::string value();
  static std::string early();
};

class ErrorTextReturnAwait {
public:
  static std::string concise();
  static std::string returned();
};

class ErrorTextReturnValue {
public:
  static std::string missing(llvm::StringRef label, llvm::StringRef type);
  static std::string wrong(llvm::StringRef label, llvm::StringRef type, llvm::StringRef given);
  static std::string promise(llvm::StringRef given, llvm::StringRef label, llvm::StringRef type);
  static std::string notVoid(llvm::StringRef label, llvm::StringRef type);
};

}
