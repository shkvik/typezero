#pragma once

#include "llvm/ADT/StringRef.h"
#include <string>

namespace tz {

enum class ConstructorCallsOverriddenCallErrorCode {
  CONSTRUCTOR_CALLS_OVERRIDE,
  CONSTRUCTOR_REACHES_OVERRIDE,
  INITIALIZERS_CALL_OVERRIDE,
  INITIALIZERS_REACH_OVERRIDE,
};

struct ConstructorCallsOverriddenCallError {
  ConstructorCallsOverriddenCallErrorCode code;
  std::string message;
};

class ErrorTextOverriddenCall {
public:
  static std::string ctorCall(llvm::StringRef className, llvm::StringRef method,
                              llvm::StringRef child);
  static std::string ctorReach(llvm::StringRef className, llvm::StringRef method,
                               llvm::StringRef child);
  static std::string fieldsCall(llvm::StringRef className, llvm::StringRef method,
                                llvm::StringRef child);
  static std::string fieldsReach(llvm::StringRef className, llvm::StringRef method,
                                 llvm::StringRef child);

private:
  static std::string overridden(llvm::StringRef caller, llvm::StringRef method,
                                llvm::StringRef path, llvm::StringRef child);
};

}
