#pragma once

#include "llvm/ADT/StringRef.h"
#include <string>

namespace tz {

enum class FsPromisesCheckerPromiseFunctionErrorCode {
  NOT_A_FUNCTION,
  NO_FILE_HANDLE,
};

struct FsPromisesCheckerPromiseFunctionError {
  FsPromisesCheckerPromiseFunctionErrorCode code;
  std::string message;
};

class ErrorTextFsPromises {
public:
  static std::string constants();
  static std::string open();
  static std::string notYet(llvm::StringRef name);
};

}
