#pragma once

#include "llvm/ADT/StringRef.h"
#include <string>

namespace tz {

class ErrorTextProcessEvent {
public:
  static std::string noThrown();
};

enum class ProcessCheckerListenedSignalErrorCode {
  NOT_A_SIGNAL,
};

struct ProcessCheckerListenedSignalError {
  ProcessCheckerListenedSignalErrorCode code;
  std::string message;
};

class ErrorTextProcessCall {
public:
  static std::string onlyNow(llvm::StringRef object, llvm::StringRef name);
  static std::string ttyValue(llvm::StringRef stream);
  static std::string streamCall(llvm::StringRef stream, llvm::StringRef name);
  static std::string notFunc(llvm::StringRef name);
  static std::string notYet(llvm::StringRef name);
  static std::string signal(llvm::StringRef name, llvm::StringRef event);
};

class ErrorTextProcessStream {
public:
  static std::string name(bool out);
};

class ErrorTextProcessMember {
public:
  static std::string writeCall(llvm::StringRef stream);
  static std::string streamPart(llvm::StringRef stream, llvm::StringRef name);
  static std::string perfNow();
  static std::string perfOther(llvm::StringRef name);
  static std::string onlyCalled(llvm::StringRef name);
  static std::string streamUse(llvm::StringRef name);
};

class ErrorTextProcessAssign {
public:
  static std::string exitCode();
  static std::string assigned(llvm::StringRef name);
  static std::string changed(llvm::StringRef name);
};

}
