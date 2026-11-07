#pragma once

#include "llvm/ADT/StringRef.h"
#include <string>

namespace tz {

enum class TimerCheckerTaskCallbackErrorCode {
  NOT_A_FUNCTION,
  CALLED_WITH_NOTHING,
};

struct TimerCheckerTaskCallbackError {
  TimerCheckerTaskCallbackErrorCode code;
  std::string message;
};

class ErrorTextTaskCallback {
public:
  static std::string notFunction(llvm::StringRef fn, llvm::StringRef type);
  static std::string nothing(llvm::StringRef fn, llvm::StringRef type);
};

enum class TimerCheckerCheckAsyncGlobalErrorCode {
  NOT_A_TIMER,
  ALREADY_REPORTED,
  TIMEOUT_CALLBACK_ARGUMENTS,
  IMMEDIATE_CALLBACK_ARGUMENTS,
  WRONG_ARITY,
};

struct TimerCheckerCheckAsyncGlobalError {
  TimerCheckerCheckAsyncGlobalErrorCode code;
  std::string message;
};

class ErrorTextAsyncGlobal {
public:
  static std::string callbackArg();
};

enum class TimerCheckerCheckNextTickErrorCode {
  CALLBACK_ARGUMENTS,
  WRONG_ARITY,
};

struct TimerCheckerCheckNextTickError {
  TimerCheckerCheckNextTickErrorCode code;
  std::string message;
};

enum class TimerCheckerCheckTimerMethodErrorCode {
  NO_SUCH_METHOD,
  ALREADY_REPORTED,
  WRONG_ARITY,
};

struct TimerCheckerCheckTimerMethodError {
  TimerCheckerCheckTimerMethodErrorCode code;
  std::string message;
};

class ErrorTextTimerMethod {
public:
  static std::string noMethod(llvm::StringRef name, llvm::StringRef type, bool immediate);
};

enum class TimerCheckerCheckTimersPromisesCallErrorCode {
  SCHEDULER_ITSELF,
  ALREADY_REPORTED,
  WRONG_ARITY,
  VOID_VALUE,
};

struct TimerCheckerCheckTimersPromisesCallError {
  TimerCheckerCheckTimersPromisesCallErrorCode code;
  std::string message;
};

class ErrorTextTimersPromises {
public:
  static std::string scheduler();
  static std::string voidValue(llvm::StringRef fn);
};

enum class TimerCheckerCheckSchedulerCallErrorCode {
  ALREADY_REPORTED,
  WRONG_ARITY,
  NO_SUCH_FUNCTION,
  TIMER_FAILED,
};

struct TimerCheckerCheckSchedulerCallError {
  TimerCheckerCheckSchedulerCallErrorCode code;
  std::string message;
};

class ErrorTextScheduler {
public:
  static std::string noFunction(llvm::StringRef name);
};

}
