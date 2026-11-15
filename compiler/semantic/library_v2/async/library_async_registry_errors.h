#pragma once

#include "llvm/ADT/StringRef.h"
#include <string>

namespace tz {

enum class LibraryAsyncRegistryResolvePromiseTypeErrorCode {
  NOT_A_PROMISE,
  WRONG_TYPE_ARGUMENTS,
};

struct LibraryAsyncRegistryResolvePromiseTypeError {
  LibraryAsyncRegistryResolvePromiseTypeErrorCode code;
  std::string message;
};

class ErrorTextPromiseType {
public:
  static std::string typeArgs(llvm::StringRef name);
  static std::string notPromise(llvm::StringRef type);
};

enum class LibraryAsyncRegistryPromiseDeclaredErrorCode {
  NOT_A_PROMISE,
};

struct LibraryAsyncRegistryPromiseDeclaredError {
  LibraryAsyncRegistryPromiseDeclaredErrorCode code;
  std::string message;
};

class ErrorTextAsyncReturns {
public:
  static std::string promise(llvm::StringRef given, llvm::StringRef fn, llvm::StringRef value);
};

enum class LibraryAsyncRegistryPromiseMatchesErrorCode {
  OTHER_PROMISE_TYPE,
};

struct LibraryAsyncRegistryPromiseMatchesError {
  LibraryAsyncRegistryPromiseMatchesErrorCode code;
  std::string message;
};

enum class LibraryAsyncRegistryAsyncContextErrorCode {
  AWAIT_NOT_ASYNC,
  FOR_AWAIT_NOT_ASYNC,
};

struct LibraryAsyncRegistryAsyncContextError {
  LibraryAsyncRegistryAsyncContextErrorCode code;
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

enum class LibraryAsyncRegistryAwaitableErrorCode {
  MIXED_PROMISE,
};

struct LibraryAsyncRegistryAwaitableError {
  LibraryAsyncRegistryAwaitableErrorCode code;
  std::string message;
};

class ErrorTextAwaited {
public:
  static std::string mixed(llvm::StringRef type);
};

enum class LibraryAsyncRegistryAwaitedElementErrorCode {
  MAY_BE_UNDEFINED,
  RECORD_NOT_ITERABLE,
  NOT_ASYNC_ITERABLE,
  ALREADY_REPORTED,
};

struct LibraryAsyncRegistryAwaitedElementError {
  LibraryAsyncRegistryAwaitedElementErrorCode code;
  std::string message;
};

class ErrorTextAwaitedElement {
public:
  static std::string undefined(llvm::StringRef type);
  static std::string record();
  static std::string notAsync(llvm::StringRef type);
};

enum class LibraryAsyncRegistryBindingTypeErrorCode {
  HAS_TYPE_ANNOTATION,
};

struct LibraryAsyncRegistryBindingTypeError {
  LibraryAsyncRegistryBindingTypeErrorCode code;
  std::string message;
};

class ErrorTextBindingType {
public:
  static std::string annotated();
};

enum class LibraryAsyncRegistryBindingShapeErrorCode {
  NOT_AN_ARRAY,
};

struct LibraryAsyncRegistryBindingShapeError {
  LibraryAsyncRegistryBindingShapeErrorCode code;
  std::string message;
};

class ErrorTextBindingShape {
public:
  static std::string notArray(llvm::StringRef type);
};

enum class LibraryAsyncDiscardedPromiseErrorCode {
  PROMISES_DROPPED,
  CALLBACK_DROPS,
  RESULT_UNCHECKED,
  NOT_AWAITED,
};

struct LibraryAsyncDiscardedPromiseError {
  LibraryAsyncDiscardedPromiseErrorCode code;
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

enum class LibraryAsyncRegistryValueTypeErrorCode {
  TOO_MANY_TYPE_ARGUMENTS,
  VALUE_NOT_INFERRED,
};

struct LibraryAsyncRegistryValueTypeError {
  LibraryAsyncRegistryValueTypeErrorCode code;
  std::string message;
};

class ErrorTextPromiseValue {
public:
  static std::string typeArgs();
  static std::string notInferred();
};

enum class LibraryAsyncRegistryExecutorShapeErrorCode {
  REJECT_PARAMETER,
};

struct LibraryAsyncRegistryExecutorShapeError {
  LibraryAsyncRegistryExecutorShapeErrorCode code;
  std::string message;
};

class ErrorTextExecutor {
public:
  static std::string rejects();
};

enum class LibraryAsyncRegistryCheckPromiseMethodErrorCode {
  ALREADY_REPORTED,
  RESULT_METHOD,
  NO_CATCH,
  NO_FINALLY,
  THEN_TWO_FUNCTIONS,
  WRONG_ARITY,
  NO_SUCH_METHOD,
};

struct LibraryAsyncRegistryCheckPromiseMethodError {
  LibraryAsyncRegistryCheckPromiseMethodErrorCode code;
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

enum class LibraryAsyncRegistryStaticTypeArgsErrorCode {
  TYPE_ARGUMENTS_UNUSED,
};

struct LibraryAsyncRegistryStaticTypeArgsError {
  LibraryAsyncRegistryStaticTypeArgsErrorCode code;
  std::string message;
};

class ErrorTextStaticTypeArgs {
public:
  static std::string resolvers();
};

enum class LibraryAsyncRegistryNotVoidErrorCode {
  VOID_VALUE,
};

struct LibraryAsyncRegistryNotVoidError {
  LibraryAsyncRegistryNotVoidErrorCode code;
  std::string message;
};

class ErrorTextPromiseVoid {
public:
  static std::string given(llvm::StringRef fn);
};

enum class LibraryAsyncRegistryResolverTypeArgsErrorCode {
  TOO_MANY_TYPE_ARGUMENTS,
};

struct LibraryAsyncRegistryResolverTypeArgsError {
  LibraryAsyncRegistryResolverTypeArgsErrorCode code;
  std::string message;
};

enum class LibraryAsyncRegistryResolverValueErrorCode {
  VALUE_TYPE_UNKNOWN,
  ALREADY_REPORTED,
};

struct LibraryAsyncRegistryResolverValueError {
  LibraryAsyncRegistryResolverValueErrorCode code;
  std::string message;
};

class ErrorTextResolverValue {
public:
  static std::string typeArgs();
  static std::string unknown();
};

enum class LibraryAsyncRegistryRaceJoinErrorCode {
  TYPES_DIFFER,
};

struct LibraryAsyncRegistryRaceJoinError {
  LibraryAsyncRegistryRaceJoinErrorCode code;
  std::string message;
};

class ErrorTextRaceJoin {
public:
  static std::string differ(llvm::StringRef fn, llvm::StringRef type, llvm::StringRef other);
};

enum class LibraryAsyncRegistryAllValuesErrorCode {
  VOID_PROMISE,
};

struct LibraryAsyncRegistryAllValuesError {
  LibraryAsyncRegistryAllValuesErrorCode code;
  std::string message;
};

class ErrorTextAllValues {
public:
  static std::string voidValue();
};

enum class LibraryAsyncRegistryEmptyListErrorCode {
  ELEMENT_TYPE_UNKNOWN,
};

struct LibraryAsyncRegistryEmptyListError {
  LibraryAsyncRegistryEmptyListErrorCode code;
  std::string message;
};

enum class LibraryAsyncRegistryPlainValueErrorCode {
  MAY_BE_PROMISE,
};

struct LibraryAsyncRegistryPlainValueError {
  LibraryAsyncRegistryPlainValueErrorCode code;
  std::string message;
};

enum class LibraryAsyncRegistryPromiseArrayErrorCode {
  NOT_AN_ARRAY,
};

struct LibraryAsyncRegistryPromiseArrayError {
  LibraryAsyncRegistryPromiseArrayErrorCode code;
  std::string message;
};

class ErrorTextPromiseList {
public:
  static std::string valuesLabel();
  static std::string noType(llvm::StringRef fn);
  static std::string mayBe(llvm::StringRef fn, llvm::StringRef type);
  static std::string notArray(llvm::StringRef fn, llvm::StringRef type, bool iterable,
                              llvm::StringRef values);
};

class ErrorTextNoReject {
public:
  static std::string rejected();
  static std::string settled();
  static std::string any();
};

class ErrorTextPromiseStatic {
public:
  static std::string unknown(llvm::StringRef name);
};

enum class LibraryAsyncRegistryTaskCallbackErrorCode {
  NOT_A_FUNCTION,
  CALLED_WITH_NOTHING,
};

struct LibraryAsyncRegistryTaskCallbackError {
  LibraryAsyncRegistryTaskCallbackErrorCode code;
  std::string message;
};

class ErrorTextTaskCallback {
public:
  static std::string notFunction(llvm::StringRef fn, llvm::StringRef type);
  static std::string nothing(llvm::StringRef fn, llvm::StringRef type);
};

enum class LibraryAsyncRegistryCheckAsyncGlobalErrorCode {
  NOT_A_TIMER,
  ALREADY_REPORTED,
  TIMEOUT_CALLBACK_ARGUMENTS,
  IMMEDIATE_CALLBACK_ARGUMENTS,
  WRONG_ARITY,
};

struct LibraryAsyncRegistryCheckAsyncGlobalError {
  LibraryAsyncRegistryCheckAsyncGlobalErrorCode code;
  std::string message;
};

class ErrorTextAsyncGlobal {
public:
  static std::string callbackArg();
};

enum class LibraryAsyncRegistryCheckNextTickErrorCode {
  CALLBACK_ARGUMENTS,
  WRONG_ARITY,
};

struct LibraryAsyncRegistryCheckNextTickError {
  LibraryAsyncRegistryCheckNextTickErrorCode code;
  std::string message;
};

enum class LibraryAsyncRegistryCheckTimerMethodErrorCode {
  NO_SUCH_METHOD,
  ALREADY_REPORTED,
  WRONG_ARITY,
};

struct LibraryAsyncRegistryCheckTimerMethodError {
  LibraryAsyncRegistryCheckTimerMethodErrorCode code;
  std::string message;
};

class ErrorTextTimerMethod {
public:
  static std::string noMethod(llvm::StringRef name, llvm::StringRef type, bool immediate);
};

enum class LibraryAsyncRegistryCheckTimersPromisesCallErrorCode {
  SCHEDULER_ITSELF,
  ALREADY_REPORTED,
  WRONG_ARITY,
  VOID_VALUE,
};

struct LibraryAsyncRegistryCheckTimersPromisesCallError {
  LibraryAsyncRegistryCheckTimersPromisesCallErrorCode code;
  std::string message;
};

class ErrorTextTimersPromises {
public:
  static std::string scheduler();
  static std::string voidValue(llvm::StringRef fn);
};

enum class LibraryAsyncRegistryCheckSchedulerCallErrorCode {
  ALREADY_REPORTED,
  WRONG_ARITY,
  NO_SUCH_FUNCTION,
  TIMER_FAILED,
};

struct LibraryAsyncRegistryCheckSchedulerCallError {
  LibraryAsyncRegistryCheckSchedulerCallErrorCode code;
  std::string message;
};

class ErrorTextScheduler {
public:
  static std::string noFunction(llvm::StringRef name);
};

}
