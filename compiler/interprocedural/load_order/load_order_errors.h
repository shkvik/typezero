#pragma once

#include "llvm/ADT/StringRef.h"
#include <string>

namespace tz {

enum class LoadOrderEarlyReadErrorCode {
  USED_BEFORE_DECLARED,
};

struct LoadOrderEarlyReadError {
  LoadOrderEarlyReadErrorCode code;
  std::string message;
};

class ErrorTextEarlyRead {
public:
  static std::string sameFile(llvm::StringRef name);
};

enum class LoadOrderLateUseErrorCode {
  USED_BEFORE_DECLARED,
  READ_BEFORE_LOADED,
};

struct LoadOrderLateUseError {
  LoadOrderLateUseErrorCode code;
  std::string message;
};

class ErrorTextLateUse {
public:
  static std::string sameFile(llvm::StringRef callee, llvm::StringRef var);
  static std::string otherFile(llvm::StringRef file, llvm::StringRef callee, llvm::StringRef var,
                               llvm::StringRef home, llvm::StringRef chain);
  static std::string usedHere(llvm::StringRef var);
  static std::string declared(llvm::StringRef var);
  static std::string function(llvm::StringRef name);
  static std::string creation(llvm::StringRef className);
  static std::string method(llvm::StringRef className, llvm::StringRef name);
};

}
