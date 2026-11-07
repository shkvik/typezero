#pragma once

#include "llvm/ADT/StringRef.h"
#include <string>

namespace tz {

enum class FunctionValueCheckerExtraArgumentsErrorCode {
  MORE_ARGUMENTS,
};

struct FunctionValueCheckerExtraArgumentsError {
  FunctionValueCheckerExtraArgumentsErrorCode code;
  std::string message;
};

class ErrorTextExtraArguments {
public:
  static std::string passed(llvm::StringRef function, llvm::StringRef argument,
                            llvm::StringRef ordinal, llvm::StringRef method,
                            llvm::StringRef passes);
};

enum class FunctionValueCheckerAsyncCallbackErrorCode {
  REPLACE_GETS_PROMISE,
  TEST_GETS_PROMISE,
  VALUE_GETS_PROMISE,
};

struct FunctionValueCheckerAsyncCallbackError {
  FunctionValueCheckerAsyncCallbackErrorCode code;
  std::string message;
};

class ErrorTextAsyncCallback {
public:
  static std::string replace(llvm::StringRef method, llvm::StringRef type);
  static std::string test(llvm::StringRef method, llvm::StringRef type);
  static std::string value(llvm::StringRef method, llvm::StringRef type);
};

}
