#pragma once

#include "llvm/ADT/StringRef.h"
#include <string>

namespace tz {

enum class FunctionCheckerTopLevelGenericErrorCode {
  NESTED_GENERIC,
};

struct FunctionCheckerTopLevelGenericError {
  FunctionCheckerTopLevelGenericErrorCode code;
  std::string message;
};

class ErrorTextFunctionDecl {
public:
  static std::string nested();
  static std::string decorator();
  static std::string needsType(llvm::StringRef label);
  static std::string defaultNeedsType(llvm::StringRef label);
  static std::string arrowThis();
  static std::string methodThis();
  static std::string restArray(llvm::StringRef name);
  static std::string required();
};

enum class FunctionCheckerParamDecoratorsErrorCode {
  DECORATOR_NOT_ALLOWED,
};

struct FunctionCheckerParamDecoratorsError {
  FunctionCheckerParamDecoratorsErrorCode code;
  std::string message;
};

enum class FunctionCheckerThisParamErrorCode {
  ARROW_THIS,
  METHOD_THIS,
};

struct FunctionCheckerThisParamError {
  FunctionCheckerThisParamErrorCode code;
  std::string message;
};

enum class FunctionCheckerRestTypeErrorCode {
  REST_NOT_ARRAY,
};

struct FunctionCheckerRestTypeError {
  FunctionCheckerRestTypeErrorCode code;
  std::string message;
};

enum class FunctionCheckerRequiredOrderErrorCode {
  REQUIRED_AFTER_OPTIONAL,
};

struct FunctionCheckerRequiredOrderError {
  FunctionCheckerRequiredOrderErrorCode code;
  std::string message;
};

enum class FunctionCheckerSuperCalledErrorCode {
  NO_SUPER_CALL,
};

struct FunctionCheckerSuperCalledError {
  FunctionCheckerSuperCalledErrorCode code;
  std::string message;
};

class ErrorTextFunctionBody {
public:
  static std::string noSuper(llvm::StringRef cls, llvm::StringRef parent);
  static std::string forward(llvm::StringRef label, llvm::StringRef used);
  static std::string noReturn(llvm::StringRef label, llvm::StringRef type);
};

enum class FunctionCheckerParamTypeKnownErrorCode {
  NEEDS_TYPE,
  DEFAULT_NOT_LITERAL,
};

struct FunctionCheckerParamTypeKnownError {
  FunctionCheckerParamTypeKnownErrorCode code;
  std::string message;
};

enum class FunctionCheckerDefaultOrderErrorCode {
  FORWARD_REFERENCE,
};

struct FunctionCheckerDefaultOrderError {
  FunctionCheckerDefaultOrderErrorCode code;
  std::string message;
};

enum class FunctionCheckerReturnsEverywhereErrorCode {
  MISSING_RETURN,
};

struct FunctionCheckerReturnsEverywhereError {
  FunctionCheckerReturnsEverywhereErrorCode code;
  std::string message;
};

class ErrorTextFunctionName {
public:
  static std::string quoted(llvm::StringRef name);
  static std::string label(llvm::StringRef name);
};

}
