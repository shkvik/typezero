#pragma once

#include "llvm/ADT/StringRef.h"
#include <string>

namespace tz {

enum class GeneratorCheckerResolveIteratorTypeErrorCode {
  NOT_AN_ITERATOR,
  RESULT_TYPE_ARGUMENTS,
  ITERABLE_TYPE,
  WRONG_TYPE_ARGUMENTS,
};

struct GeneratorCheckerResolveIteratorTypeError {
  GeneratorCheckerResolveIteratorTypeErrorCode code;
  std::string message;
};

class ErrorTextIteratorType {
public:
  static std::string resultArgs();
  static std::string iterable();
  static std::string typeArgs(llvm::StringRef name);
};

enum class GeneratorCheckerGeneratorReturnTypeErrorCode {
  ASYNC_NEEDS_ASYNC_ITERATOR,
  NEEDS_ITERATOR,
};

struct GeneratorCheckerGeneratorReturnTypeError {
  GeneratorCheckerGeneratorReturnTypeErrorCode code;
  std::string message;
};

class ErrorTextGeneratorType {
public:
  static std::string async(llvm::StringRef yielded);
  static std::string sync(llvm::StringRef declared);
};

enum class GeneratorCheckerCheckSendsNothingErrorCode {
  EXPECTS_VALUE,
};

struct GeneratorCheckerCheckSendsNothingError {
  GeneratorCheckerCheckSendsNothingErrorCode code;
  std::string message;
};

class ErrorTextSendsNothing {
public:
  static std::string expects(llvm::StringRef what, llvm::StringRef iterator, llvm::StringRef sent);
};

enum class GeneratorCheckerYieldsJoinErrorCode {
  TYPES_DIFFER,
};

struct GeneratorCheckerYieldsJoinError {
  GeneratorCheckerYieldsJoinErrorCode code;
  std::string message;
};

class ErrorTextYields {
public:
  static std::string differ(llvm::StringRef fn, llvm::StringRef here, llvm::StringRef elsewhere);
  static std::string unknown(llvm::StringRef fn, llvm::StringRef why);
  static std::string noYield();
  static std::string onlyUndef();
};

enum class GeneratorCheckerReturnsJoinErrorCode {
  TYPES_DIFFER,
};

struct GeneratorCheckerReturnsJoinError {
  GeneratorCheckerReturnsJoinErrorCode code;
  std::string message;
};

enum class GeneratorCheckerYieldsKnownErrorCode {
  NOTHING_YIELDED,
};

struct GeneratorCheckerYieldsKnownError {
  GeneratorCheckerYieldsKnownErrorCode code;
  std::string message;
};

enum class GeneratorCheckerAsyncSentErrorCode {
  SENT_TO_ASYNC,
};

struct GeneratorCheckerAsyncSentError {
  GeneratorCheckerAsyncSentErrorCode code;
  std::string message;
};

class ErrorTextYield {
public:
  static std::string asyncSent(llvm::StringRef yielded);
  static std::string mixed(llvm::StringRef type);
  static std::string promise(llvm::StringRef given, llvm::StringRef fn, llvm::StringRef yielded);
  static std::string noValue(llvm::StringRef fn, llvm::StringRef yielded);
  static std::string notAsync(llvm::StringRef type);
  static std::string otherType(llvm::StringRef element, llvm::StringRef fn,
                               llvm::StringRef yielded);
  static std::string notIter(llvm::StringRef type);
  static std::string sends(llvm::StringRef type, llvm::StringRef expects, llvm::StringRef fn,
                           llvm::StringRef gets);
  static std::string untyped(llvm::StringRef yielded);
  static std::string nothing();
};

enum class GeneratorCheckerPromiseYieldErrorCode {
  MIXED_PROMISE,
  OTHER_TYPE,
};

struct GeneratorCheckerPromiseYieldError {
  GeneratorCheckerPromiseYieldErrorCode code;
  std::string message;
};

enum class GeneratorCheckerValueYieldedErrorCode {
  UNDEFINED_YIELD,
};

struct GeneratorCheckerValueYieldedError {
  GeneratorCheckerValueYieldedErrorCode code;
  std::string message;
};

enum class GeneratorCheckerDelegateAsyncErrorCode {
  NOT_ASYNC_ITERABLE,
  OTHER_TYPE,
};

struct GeneratorCheckerDelegateAsyncError {
  GeneratorCheckerDelegateAsyncErrorCode code;
  std::string message;
};

enum class GeneratorCheckerDelegateSyncErrorCode {
  NOT_ITERABLE,
  SENDS_DIFFER,
  OTHER_TYPE,
};

struct GeneratorCheckerDelegateSyncError {
  GeneratorCheckerDelegateSyncErrorCode code;
  std::string message;
};

enum class GeneratorCheckerSentDeclaredErrorCode {
  UNTYPED_SENT,
};

struct GeneratorCheckerSentDeclaredError {
  GeneratorCheckerSentDeclaredErrorCode code;
  std::string message;
};

}
