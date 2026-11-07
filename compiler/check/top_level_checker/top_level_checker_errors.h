#pragma once

#include "llvm/ADT/StringRef.h"
#include <string>

namespace tz {

enum class TopLevelCheckerCheckLoadedErrorCode {
  ALREADY_REPORTED,
  READ_WHILE_LOADING,
  NO_TYPE_BEFORE_CALL,
  NO_TYPE_IN_FUNCTION,
};

struct TopLevelCheckerCheckLoadedError {
  TopLevelCheckerCheckLoadedErrorCode code;
  std::string message;
};

class ErrorTextNotLoaded {
public:
  static std::string whileLoads(llvm::StringRef reader, llvm::StringRef what,
                                llvm::StringRef source, llvm::StringRef chain,
                                llvm::StringRef declared);
  static std::string beforeCall(llvm::StringRef what, llvm::StringRef reader,
                                llvm::StringRef early);
  static std::string inFunction(llvm::StringRef what, llvm::StringRef reader);
  static std::string label(llvm::StringRef name, llvm::StringRef declared, llvm::StringRef file);
  static std::string labelOf(llvm::StringRef name, llvm::StringRef file);
  static std::string quoted(llvm::StringRef name);
  static std::string pending(llvm::StringRef name, bool started);
};

enum class TopLevelCheckerCheckImportAssignErrorCode {
  IMPORTED_VARIABLE,
};

struct TopLevelCheckerCheckImportAssignError {
  TopLevelCheckerCheckImportAssignErrorCode code;
  std::string message;
};

class ErrorTextImportAssign {
public:
  static std::string imported(llvm::StringRef name);
};

}
