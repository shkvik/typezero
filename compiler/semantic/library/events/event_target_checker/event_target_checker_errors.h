#pragma once

#include "llvm/ADT/StringRef.h"
#include <string>

namespace tz {

enum class EventTargetCheckerListenerEventErrorCode {
  NOT_AN_EVENT,
};

struct EventTargetCheckerListenerEventError {
  EventTargetCheckerListenerEventErrorCode code;
  std::string message;
};

enum class EventTargetCheckerEmitterEventsErrorCode {
  NOT_AN_EMITTER,
};

struct EventTargetCheckerEmitterEventsError {
  EventTargetCheckerEmitterEventsErrorCode code;
  std::string message;
};

enum class EventTargetCheckerOneErrorValueErrorCode {
  SEVERAL_VALUES,
};

struct EventTargetCheckerOneErrorValueError {
  EventTargetCheckerOneErrorValueErrorCode code;
  std::string message;
};

class ErrorTextEventTarget {
public:
  static std::string listener(llvm::StringRef type);
};

class ErrorTextEventsFunction {
public:
  static std::string notEmitter(llvm::StringRef fn, bool target, llvm::StringRef type);
  static std::string targetOn();
  static std::string endless(llvm::StringRef name, llvm::StringRef fn);
  static std::string options();
  static std::string errorValue(llvm::StringRef fn, llvm::StringRef errors);
};

}
