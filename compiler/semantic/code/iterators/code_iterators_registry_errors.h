#pragma once

#include "llvm/ADT/StringRef.h"
#include <string>

namespace tz {

enum class CodeIteratorsRegistryGeneratorReturnTypeErrorCode {
  ASYNC_NEEDS_ASYNC_ITERATOR,
  NEEDS_ITERATOR,
};

struct CodeIteratorsRegistryGeneratorReturnTypeError {
  CodeIteratorsRegistryGeneratorReturnTypeErrorCode code;
  std::string message;
};

enum class CodeIteratorsRegistryCheckSendsNothingErrorCode {
  EXPECTS_VALUE,
};

struct CodeIteratorsRegistryCheckSendsNothingError {
  CodeIteratorsRegistryCheckSendsNothingErrorCode code;
  std::string message;
};

enum class CodeIteratorsRegistryAsyncSentErrorCode {
  SENT_TO_ASYNC,
};

struct CodeIteratorsRegistryAsyncSentError {
  CodeIteratorsRegistryAsyncSentErrorCode code;
  std::string message;
};

enum class CodeIteratorsRegistryPromiseYieldErrorCode {
  MIXED_PROMISE,
  OTHER_TYPE,
};

struct CodeIteratorsRegistryPromiseYieldError {
  CodeIteratorsRegistryPromiseYieldErrorCode code;
  std::string message;
};

enum class CodeIteratorsRegistryValueYieldedErrorCode {
  UNDEFINED_YIELD,
};

struct CodeIteratorsRegistryValueYieldedError {
  CodeIteratorsRegistryValueYieldedErrorCode code;
  std::string message;
};

enum class CodeIteratorsRegistryDelegateAsyncErrorCode {
  NOT_ASYNC_ITERABLE,
  OTHER_TYPE,
};

struct CodeIteratorsRegistryDelegateAsyncError {
  CodeIteratorsRegistryDelegateAsyncErrorCode code;
  std::string message;
};

enum class CodeIteratorsRegistryDelegateSyncErrorCode {
  NOT_ITERABLE,
  SENDS_DIFFER,
  OTHER_TYPE,
};

struct CodeIteratorsRegistryDelegateSyncError {
  CodeIteratorsRegistryDelegateSyncErrorCode code;
  std::string message;
};

enum class CodeIteratorsRegistrySentDeclaredErrorCode {
  UNTYPED_SENT,
};

struct CodeIteratorsRegistrySentDeclaredError {
  CodeIteratorsRegistrySentDeclaredErrorCode code;
  std::string message;
};

enum class CodeIteratorsRegistryIteratorGivenErrorCode {
  NOT_AN_ITERATOR,
};

struct CodeIteratorsRegistryIteratorGivenError {
  CodeIteratorsRegistryIteratorGivenErrorCode code;
  std::string message;
};

enum class CodeIteratorsRegistryAsyncIteratorGivenErrorCode {
  NOT_AN_ASYNC_ITERATOR,
};

struct CodeIteratorsRegistryAsyncIteratorGivenError {
  CodeIteratorsRegistryAsyncIteratorGivenErrorCode code;
  std::string message;
};

enum class CodeIteratorsRegistryIterableParamErrorCode {
  WRONG_ARGUMENT_COUNT,
  CONSTRUCTOR_ITERABLE,
  CLOSURE_ITERABLE,
};

struct CodeIteratorsRegistryIterableParamError {
  CodeIteratorsRegistryIterableParamErrorCode code;
  std::string message;
};

enum class CodeIteratorsRegistryResolveIteratorTypeErrorCode {
  RESULT_TYPE_ARGUMENTS,
  ITERABLE_TYPE,
  WRONG_TYPE_ARGUMENTS,
};

struct CodeIteratorsRegistryResolveIteratorTypeError {
  CodeIteratorsRegistryResolveIteratorTypeErrorCode code;
  std::string message;
};

class ErrorTextIteratorType {
public:
  static std::string resultArgs();
  static std::string iterable();
  static std::string typeArgs(llvm::StringRef name);
};

class ErrorTextGeneratorType {
public:
  static std::string async(llvm::StringRef yielded);
  static std::string sync(llvm::StringRef declared);
};

class ErrorTextSendsNothing {
public:
  static std::string destructuring();
  static std::string expects(llvm::StringRef what, llvm::StringRef iterator, llvm::StringRef sent);
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

class ErrorTextIteratorGiven {
public:
  static std::string notIterator(llvm::StringRef type, llvm::StringRef gives);
};

class ErrorTextAsyncGiven {
public:
  static std::string notAsync(llvm::StringRef type, llvm::StringRef gives);
};

class ErrorTextIterableParam {
public:
  static std::string typeArgs();
  static std::string inCtor(llvm::StringRef element);
  static std::string notTop(llvm::StringRef element);
};

}
