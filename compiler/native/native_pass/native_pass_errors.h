#pragma once

#include "llvm/ADT/StringRef.h"
#include <string>

namespace tz {

enum class NativePassErrorCode {
  TWO_ENTRIES_FIT,
  MISSING_FILE,
  UNREADABLE_LIBRARY,
  NO_SOURCE_FILES,
  SOURCE_ERRORS,
  JOIN_FAILED,
};

struct NativePassError {
  NativePassErrorCode code;
  std::string message;
};

class ErrorTextNativePass {
public:
  static std::string twoFit(llvm::StringRef name, llvm::StringRef platform);
  static std::string missing(llvm::StringRef name, llvm::StringRef what, llvm::StringRef path);
  static std::string unreadable(llvm::StringRef name, llvm::StringRef path, llvm::StringRef why);
  static std::string noFiles(llvm::StringRef name, llvm::StringRef pattern);
  static std::string compile(llvm::StringRef name, llvm::StringRef output);
  static std::string join(llvm::StringRef name);
};

}
