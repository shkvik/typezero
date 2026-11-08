#pragma once

#include "llvm/ADT/StringRef.h"
#include <string>

namespace tz {

enum class CallbackCheckerFunctionGivenErrorCode {
  NOT_A_FUNCTION,
};

struct CallbackCheckerFunctionGivenError {
  CallbackCheckerFunctionGivenErrorCode code;
  std::string message;
};

class ErrorTextCallbackArg {
public:
  static std::string notFunc(llvm::StringRef method, llvm::StringRef type);
  static std::string rest(llvm::StringRef name);
  static std::string paramType(llvm::StringRef name, llvm::StringRef param, llvm::StringRef ordinal,
                               llvm::StringRef method, llvm::StringRef given);
  static std::string extraParam(llvm::StringRef name, llvm::StringRef argName,
                                llvm::StringRef paramName, llvm::StringRef method,
                                llvm::StringRef args);
  static std::string tooFew(llvm::StringRef method, llvm::StringRef passes, llvm::StringRef name,
                            llvm::StringRef needs);
  static std::string badReturn(llvm::StringRef method, llvm::StringRef wanted, llvm::StringRef name,
                               llvm::StringRef returns);
  static std::string noCheck(llvm::StringRef name, llvm::StringRef method);
  static std::string noWait(llvm::StringRef name, llvm::StringRef method);
  static std::string argsShape(llvm::StringRef method);
  static std::string position(size_t index);
  static std::string argName(llvm::StringRef method, size_t index);
  static std::string paramName(llvm::StringRef name);
};

enum class CallbackCheckerFixedParamsErrorCode {
  REST_PARAMETER,
};

struct CallbackCheckerFixedParamsError {
  CallbackCheckerFixedParamsErrorCode code;
  std::string message;
};

enum class CallbackCheckerCallbackParamErrorCode {
  PARAM_TYPE,
  EXTRA_OPTIONAL,
};

struct CallbackCheckerCallbackParamError {
  CallbackCheckerCallbackParamErrorCode code;
  std::string message;
};

enum class CallbackCheckerArgumentsEnoughErrorCode {
  TOO_FEW_PASSED,
};

struct CallbackCheckerArgumentsEnoughError {
  CallbackCheckerArgumentsEnoughErrorCode code;
  std::string message;
};

enum class CallbackCheckerReturnMatchesErrorCode {
  WRONG_RETURN,
};

struct CallbackCheckerReturnMatchesError {
  CallbackCheckerReturnMatchesErrorCode code;
  std::string message;
};

enum class CallbackCheckerResultKeptErrorCode {
  RESULT_DROPPED,
  PROMISE_DROPPED,
};

struct CallbackCheckerResultKeptError {
  CallbackCheckerResultKeptErrorCode code;
  std::string message;
};

enum class CallbackCheckerCallbackGivenErrorCode {
  NO_FUNCTION,
};

struct CallbackCheckerCallbackGivenError {
  CallbackCheckerCallbackGivenErrorCode code;
  std::string message;
};

class ErrorTextCallbackMethod {
public:
  static std::string noFunction(llvm::StringRef name);
  static std::string thisArg(llvm::StringRef name);
  static std::string untyped(llvm::StringRef name);
  static std::string noValue(llvm::StringRef name);
  static std::string notNumber(llvm::StringRef type);
};

enum class CallbackCheckerExtraArgumentsErrorCode {
  THIS_ARGUMENT,
  TOO_MANY_ARGUMENTS,
};

struct CallbackCheckerExtraArgumentsError {
  CallbackCheckerExtraArgumentsErrorCode code;
  std::string message;
};

enum class CallbackCheckerInitialValueErrorCode {
  UNTYPED_INITIAL,
};

struct CallbackCheckerInitialValueError {
  CallbackCheckerInitialValueErrorCode code;
  std::string message;
};

enum class CallbackCheckerMappedValueErrorCode {
  NO_VALUE,
};

struct CallbackCheckerMappedValueError {
  CallbackCheckerMappedValueErrorCode code;
  std::string message;
};

enum class CallbackCheckerComparisonErrorCode {
  NOT_A_NUMBER,
};

struct CallbackCheckerComparisonError {
  CallbackCheckerComparisonErrorCode code;
  std::string message;
};

}
