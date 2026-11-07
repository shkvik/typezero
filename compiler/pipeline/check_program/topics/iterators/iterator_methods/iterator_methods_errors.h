#pragma once

#include "llvm/ADT/StringRef.h"
#include <string>

namespace tz {

enum class IteratorMethodsCheckArrayIteratorErrorCode {
  WRONG_ARITY,
};

struct IteratorMethodsCheckArrayIteratorError {
  IteratorMethodsCheckArrayIteratorErrorCode code;
  std::string message;
};

enum class IteratorMethodsCheckIteratorFromErrorCode {
  ALREADY_REPORTED,
  NOT_SUPPORTED,
  WRONG_ARITY,
  NOT_ITERABLE,
};

struct IteratorMethodsCheckIteratorFromError {
  IteratorMethodsCheckIteratorFromErrorCode code;
  std::string message;
};

class ErrorTextStaticFrom {
public:
  static std::string iterator(llvm::StringRef name);
  static std::string array(llvm::StringRef name);
};

enum class IteratorMethodsArrayStaticErrorCode {
  NOT_SUPPORTED,
};

struct IteratorMethodsArrayStaticError {
  IteratorMethodsArrayStaticErrorCode code;
  std::string message;
};

enum class IteratorMethodsIteratedSourceErrorCode {
  NOT_ITERABLE,
  ALREADY_REPORTED,
};

struct IteratorMethodsIteratedSourceError {
  IteratorMethodsIteratedSourceErrorCode code;
  std::string message;
};

class ErrorTextIterSource {
public:
  static std::string notIter(llvm::StringRef fn, llvm::StringRef type);
};

enum class IteratorMethodsCallbackValueErrorCode {
  NO_VALUE,
};

struct IteratorMethodsCallbackValueError {
  IteratorMethodsCallbackValueErrorCode code;
  std::string message;
};

class ErrorTextCallbackValue {
public:
  static std::string missing(llvm::StringRef fn);
};

enum class IteratorMethodsNextValueErrorCode {
  UNION_PROBLEM,
};

struct IteratorMethodsNextValueError {
  IteratorMethodsNextValueErrorCode code;
  std::string message;
};

class ErrorTextNextValue {
public:
  static std::string noUnion(llvm::StringRef iterator, llvm::StringRef element,
                             llvm::StringRef returned, llvm::StringRef problem);
};

enum class IteratorMethodsPlainMethodErrorCode {
  NO_THROW,
  NO_RETURN,
};

struct IteratorMethodsPlainMethodError {
  IteratorMethodsPlainMethodErrorCode code;
  std::string message;
};

class ErrorTextIteratorMethod {
public:
  static std::string noThrow();
  static std::string noReturn();
  static std::string noProperty(llvm::StringRef name, llvm::StringRef type);
  static std::string noFunction(llvm::StringRef name);
  static std::string countOf(llvm::StringRef name);
};

enum class IteratorMethodsIteratorHelperErrorCode {
  NO_SUCH_PROPERTY,
};

struct IteratorMethodsIteratorHelperError {
  IteratorMethodsIteratorHelperErrorCode code;
  std::string message;
};

enum class IteratorMethodsCallbackGivenErrorCode {
  NO_FUNCTION,
  TOO_MANY_ARGUMENTS,
};

struct IteratorMethodsCallbackGivenError {
  IteratorMethodsCallbackGivenErrorCode code;
  std::string message;
};

enum class IteratorMethodsInitialValueErrorCode {
  UNTYPED_INITIAL,
};

struct IteratorMethodsInitialValueError {
  IteratorMethodsInitialValueErrorCode code;
  std::string message;
};

class ErrorTextReduceInit {
public:
  static std::string untyped();
};

enum class IteratorMethodsFlatMapInnerErrorCode {
  NOT_ITERABLE,
};

struct IteratorMethodsFlatMapInnerError {
  IteratorMethodsFlatMapInnerErrorCode code;
  std::string message;
};

class ErrorTextFlatMap {
public:
  static std::string notIter(llvm::StringRef type);
};

}
