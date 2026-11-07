#pragma once

#include "compiler/program/types/types.h"
#include "llvm/ADT/StringRef.h"
#include <string>

namespace tz {

enum class EventMapsEventMapFitsErrorCode {
  NOT_A_MAP,
  SEVERAL_VALUES,
  SERVICE_EVENT,
};

struct EventMapsEventMapFitsError {
  EventMapsEventMapFitsErrorCode code;
  std::string message;
};

enum class EventMapsListenerThisErrorCode {
  OTHER_RECEIVER,
};

struct EventMapsListenerThisError {
  EventMapsListenerThisErrorCode code;
  std::string message;
};

class ErrorTextEventMap {
public:
  static std::string notMap(llvm::StringRef why);
  static std::string notTuple(llvm::StringRef type);
  static std::string notOne(llvm::StringRef type);
  static std::string optional(llvm::StringRef event);
  static std::string fieldTuple(llvm::StringRef event, llvm::StringRef type);
  static std::string eachEvent();
  static std::string event(llvm::StringRef name);
  static std::string several(llvm::StringRef event, llvm::StringRef args);
  static std::string service(llvm::StringRef event, llvm::StringRef type);
};

class ErrorTextEventName {
public:
  static std::string unknown(llvm::StringRef method);
  static std::string list(const Type *events);
  static std::string didYouMean(llvm::StringRef name);
  static std::string notEmitted(llvm::StringRef key, const Type *events, llvm::StringRef nearest);
  static std::string noEvent(llvm::StringRef key, const Type *events, llvm::StringRef nearest);
};

class ErrorTextListener {
public:
  static std::string thisType(llvm::StringRef self, llvm::StringRef receiver);
  static std::string asyncThis();
  static std::string notFunc(llvm::StringRef type);
  static std::string gets(llvm::StringRef event, llvm::StringRef args, llvm::StringRef type);
  static std::string getsArrow(llvm::StringRef event, llvm::StringRef args, llvm::StringRef type);
  static std::string asyncRet(llvm::StringRef type);
};

}
