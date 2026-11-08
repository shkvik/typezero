#pragma once

#include "llvm/ADT/StringRef.h"
#include <string>

namespace tz {

enum class IterableCheckerIteratorGivenErrorCode {
  NOT_AN_ITERATOR,
};

struct IterableCheckerIteratorGivenError {
  IterableCheckerIteratorGivenErrorCode code;
  std::string message;
};

class ErrorTextIteratorGiven {
public:
  static std::string notIterator(llvm::StringRef type, llvm::StringRef gives);
};

enum class IterableCheckerAsyncIteratorGivenErrorCode {
  NOT_AN_ASYNC_ITERATOR,
};

struct IterableCheckerAsyncIteratorGivenError {
  IterableCheckerAsyncIteratorGivenErrorCode code;
  std::string message;
};

class ErrorTextAsyncGiven {
public:
  static std::string notAsync(llvm::StringRef type, llvm::StringRef gives);
};

enum class IterableCheckerIterableParamErrorCode {
  NOT_ITERABLE,
  WRONG_ARGUMENT_COUNT,
  CONSTRUCTOR_ITERABLE,
  CLOSURE_ITERABLE,
};

struct IterableCheckerIterableParamError {
  IterableCheckerIterableParamErrorCode code;
  std::string message;
};

class ErrorTextIterableParam {
public:
  static std::string typeArgs();
  static std::string inCtor(llvm::StringRef element);
  static std::string notTop(llvm::StringRef element);
};

}
