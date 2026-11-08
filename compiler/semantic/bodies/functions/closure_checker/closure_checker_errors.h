#pragma once

#include "llvm/ADT/StringRef.h"
#include <string>

namespace tz {

enum class ClosureCheckerNotGenericErrorCode {
  GENERIC_FUNCTION_EXPRESSION,
};

struct ClosureCheckerNotGenericError {
  ClosureCheckerNotGenericErrorCode code;
  std::string message;
};

class ErrorTextNotGeneric {
public:
  static std::string generic();
};

enum class ClosureCheckerParamAgreesErrorCode {
  DECLARED_DIFFERENTLY,
};

struct ClosureCheckerParamAgreesError {
  ClosureCheckerParamAgreesErrorCode code;
  std::string message;
};

class ErrorTextParamAgrees {
public:
  static std::string declared(llvm::StringRef label, llvm::StringRef passed, llvm::StringRef given);
};

enum class ClosureCheckerDefaultMattersErrorCode {
  DEFAULT_NEVER_USED,
};

struct ClosureCheckerDefaultMattersError {
  ClosureCheckerDefaultMattersErrorCode code;
  std::string message;
};

class ErrorTextDefaultMatters {
public:
  static std::string unused(llvm::StringRef label);
};

enum class ClosureCheckerParamTypeKnownErrorCode {
  NEEDS_TYPE,
};

struct ClosureCheckerParamTypeKnownError {
  ClosureCheckerParamTypeKnownErrorCode code;
  std::string message;
};

class ErrorTextParamTypeKnown {
public:
  static std::string needsType(llvm::StringRef label);
};

enum class ClosureCheckerThisParamErrorCode {
  ARROW_THIS_PARAMETER,
};

struct ClosureCheckerThisParamError {
  ClosureCheckerThisParamErrorCode code;
  std::string message;
};

class ErrorTextThisParam {
public:
  static std::string arrowThis();
};

enum class ClosureCheckerRestParamErrorCode {
  REST_NOT_ARRAY,
};

struct ClosureCheckerRestParamError {
  ClosureCheckerRestParamErrorCode code;
  std::string message;
};

class ErrorTextRestParam {
public:
  static std::string notArray(llvm::StringRef name);
};

enum class ClosureCheckerParamOrderErrorCode {
  REQUIRED_AFTER_OPTIONAL,
};

struct ClosureCheckerParamOrderError {
  ClosureCheckerParamOrderErrorCode code;
  std::string message;
};

class ErrorTextParamOrder {
public:
  static std::string order();
};

enum class ClosureCheckerPromiseAwaitedErrorCode {
  PROMISE_DROPPED,
};

struct ClosureCheckerPromiseAwaitedError {
  ClosureCheckerPromiseAwaitedErrorCode code;
  std::string message;
};

class ErrorTextPromiseAwaited {
public:
  static std::string dropped();
};

}
