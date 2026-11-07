#pragma once

#include "llvm/ADT/StringRef.h"
#include <string>

namespace tz {

enum class AsyncCheckerResolvePromiseTypeErrorCode {
  NOT_A_PROMISE,
  WRONG_TYPE_ARGUMENTS,
};

struct AsyncCheckerResolvePromiseTypeError {
  AsyncCheckerResolvePromiseTypeErrorCode code;
  std::string message;
};

class ErrorTextPromiseType {
public:
  static std::string typeArgs(llvm::StringRef name);
  static std::string notPromise(llvm::StringRef type);
};

enum class AsyncCheckerPromiseDeclaredErrorCode {
  NOT_A_PROMISE,
};

struct AsyncCheckerPromiseDeclaredError {
  AsyncCheckerPromiseDeclaredErrorCode code;
  std::string message;
};

enum class AsyncCheckerReturnsJoinErrorCode {
  TYPES_DIFFER,
};

struct AsyncCheckerReturnsJoinError {
  AsyncCheckerReturnsJoinErrorCode code;
  std::string message;
};

class ErrorTextAsyncReturns {
public:
  static std::string differ(llvm::StringRef fn, llvm::StringRef here, llvm::StringRef elsewhere);
  static std::string promise(llvm::StringRef given, llvm::StringRef fn, llvm::StringRef value);
};

enum class AsyncCheckerPromiseMatchesErrorCode {
  OTHER_PROMISE_TYPE,
};

struct AsyncCheckerPromiseMatchesError {
  AsyncCheckerPromiseMatchesErrorCode code;
  std::string message;
};

enum class AsyncCheckerAsyncContextErrorCode {
  AWAIT_NOT_ASYNC,
  FOR_AWAIT_NOT_ASYNC,
};

struct AsyncCheckerAsyncContextError {
  AsyncCheckerAsyncContextErrorCode code;
  std::string message;
};

class ErrorTextAsyncContext {
public:
  static std::string await(llvm::StringRef hint);
  static std::string forAwait();
  static std::string arrowHint();
  static std::string methodHint(llvm::StringRef name);
  static std::string namedHint(llvm::StringRef name);
  static std::string plainHint();
  static std::string awaitLoop();
};

enum class AsyncCheckerAwaitableErrorCode {
  MIXED_PROMISE,
};

struct AsyncCheckerAwaitableError {
  AsyncCheckerAwaitableErrorCode code;
  std::string message;
};

class ErrorTextAwaited {
public:
  static std::string mixed(llvm::StringRef type);
};

enum class AsyncCheckerAwaitedElementErrorCode {
  MAY_BE_UNDEFINED,
  RECORD_NOT_ITERABLE,
  NOT_ASYNC_ITERABLE,
  ALREADY_REPORTED,
};

struct AsyncCheckerAwaitedElementError {
  AsyncCheckerAwaitedElementErrorCode code;
  std::string message;
};

class ErrorTextAwaitedElement {
public:
  static std::string undefined(llvm::StringRef type);
  static std::string record();
  static std::string notAsync(llvm::StringRef type);
};

enum class AsyncCheckerBindingTypeErrorCode {
  HAS_TYPE_ANNOTATION,
};

struct AsyncCheckerBindingTypeError {
  AsyncCheckerBindingTypeErrorCode code;
  std::string message;
};

class ErrorTextBindingType {
public:
  static std::string annotated();
};

enum class AsyncCheckerBindingShapeErrorCode {
  NOT_AN_ARRAY,
};

struct AsyncCheckerBindingShapeError {
  AsyncCheckerBindingShapeErrorCode code;
  std::string message;
};

class ErrorTextBindingShape {
public:
  static std::string notArray(llvm::StringRef type);
};

enum class AsyncCheckerCheckDiscardedPromiseErrorCode {
  PROMISES_DROPPED,
  CALLBACK_DROPS,
  RESULT_UNCHECKED,
  NOT_AWAITED,
};

struct AsyncCheckerCheckDiscardedPromiseError {
  AsyncCheckerCheckDiscardedPromiseErrorCode code;
  std::string message;
};

class ErrorTextDiscarded {
public:
  static std::string mapped();
  static std::string callback();
  static std::string result(llvm::StringRef text);
  static std::string plain();
  static std::string withText(llvm::StringRef text);
};

}
