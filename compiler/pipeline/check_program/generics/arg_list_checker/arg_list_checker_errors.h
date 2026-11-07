#pragma once

#include "llvm/ADT/StringRef.h"
#include <string>

namespace tz {

enum class ArgListCheckerRestListValueErrorCode {
  REST_LIST_AS_VALUE,
};

struct ArgListCheckerRestListValueError {
  ArgListCheckerRestListValueErrorCode code;
  std::string message;
};

class ErrorTextRestList {
public:
  static std::string spreadOnly(llvm::StringRef list, llvm::StringRef type);
};

enum class ArgListCheckerListArgumentErrorCode {
  TOO_MANY_ARGUMENTS,
  SPREAD_MISMATCH,
  LIST_NOT_SPREAD,
};

struct ArgListCheckerListArgumentError {
  ArgListCheckerListArgumentErrorCode code;
  std::string message;
};

class ErrorTextListArgument {
public:
  static std::string tooMany(llvm::StringRef list);
  static std::string spreadWrong(llvm::StringRef spread, llvm::StringRef list, llvm::StringRef part,
                                 bool partIsList);
  static std::string notSpread(llvm::StringRef part);
};

enum class ArgListCheckerListFilledErrorCode {
  MISSING_ARGUMENTS,
};

struct ArgListCheckerListFilledError {
  ArgListCheckerListFilledErrorCode code;
  std::string message;
};

class ErrorTextListFilled {
public:
  static std::string missing(llvm::StringRef list, llvm::StringRef part, bool partIsList);
};

}
