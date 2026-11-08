#pragma once

#include "llvm/ADT/StringRef.h"
#include <string>

namespace tz {

enum class PromiseCheckerValueTypeErrorCode {
  TOO_MANY_TYPE_ARGUMENTS,
  VALUE_NOT_INFERRED,
};

struct PromiseCheckerValueTypeError {
  PromiseCheckerValueTypeErrorCode code;
  std::string message;
};

class ErrorTextPromiseValue {
public:
  static std::string typeArgs();
  static std::string notInferred();
};

enum class PromiseCheckerExecutorShapeErrorCode {
  REJECT_PARAMETER,
};

struct PromiseCheckerExecutorShapeError {
  PromiseCheckerExecutorShapeErrorCode code;
  std::string message;
};

class ErrorTextExecutor {
public:
  static std::string rejects();
};

enum class PromiseCheckerCheckPromiseMethodErrorCode {
  ALREADY_REPORTED,
  RESULT_METHOD,
  NO_CATCH,
  NO_FINALLY,
  THEN_TWO_FUNCTIONS,
  WRONG_ARITY,
  NO_SUCH_METHOD,
};

struct PromiseCheckerCheckPromiseMethodError {
  PromiseCheckerCheckPromiseMethodErrorCode code;
  std::string message;
};

class ErrorTextPromiseMethod {
public:
  static std::string onResult(llvm::StringRef name, llvm::StringRef promise);
  static std::string noCatch();
  static std::string noFinally();
  static std::string thenTwo();
  static std::string noMethod(llvm::StringRef name, llvm::StringRef type);
};

}
