#pragma once

#include "llvm/ADT/StringRef.h"
#include <string>

namespace tz {

enum class KnownModulesFindErrorCode {
  NO_MODULE,
  NOT_YET,
};

struct KnownModulesFindError {
  KnownModulesFindErrorCode code;
  std::string message;
};

class ErrorTextBuiltinModule {
public:
  static std::string none(llvm::StringRef path);
  static std::string notYet(llvm::StringRef path);
};

}
