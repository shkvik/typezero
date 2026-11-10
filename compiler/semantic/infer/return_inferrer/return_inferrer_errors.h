#pragma once

#include "llvm/ADT/StringRef.h"
#include <string>

namespace tz {

enum class ReturnInferrerEarlyCallErrorCode {
  CALLED_WHILE_LOADING,
};

struct ReturnInferrerEarlyCallError {
  ReturnInferrerEarlyCallErrorCode code;
  std::string message;
};

class ErrorTextEarlyCall {
public:
  static std::string importBack(llvm::StringRef function);
};

enum class ReturnInferrerCycleTypeErrorCode {
  DIFFERENT_TYPES,
  ONLY_SELF_CALLS,
  ONLY_CALLED_BACK,
};

struct ReturnInferrerCycleTypeError {
  ReturnInferrerCycleTypeErrorCode code;
  std::string message;
};

class ErrorTextCycleType {
public:
  static std::string differ(llvm::StringRef function);
  static std::string selfOnly(llvm::StringRef function);
  static std::string calledBack(llvm::StringRef function);
};

enum class ReturnInferrerCommonReturnErrorCode {
  DIFFERENT_TYPES,
};

struct ReturnInferrerCommonReturnError {
  ReturnInferrerCommonReturnErrorCode code;
  std::string message;
};

class ErrorTextCommonReturn {
public:
  static std::string differ(llvm::StringRef function, llvm::StringRef here,
                            llvm::StringRef elsewhere);
};

enum class ReturnInferrerDefinedReturnErrorCode {
  ONLY_UNDEFINED,
};

struct ReturnInferrerDefinedReturnError {
  ReturnInferrerDefinedReturnErrorCode code;
  std::string message;
};

class ErrorTextDefinedReturn {
public:
  static std::string undefined(llvm::StringRef function);
};

}
