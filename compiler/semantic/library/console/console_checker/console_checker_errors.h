#pragma once

#include "llvm/ADT/StringRef.h"
#include <string>

namespace tz {

enum class ConsoleCheckerPrintableErrorCode {
  VOID_VALUE,
};

struct ConsoleCheckerPrintableError {
  ConsoleCheckerPrintableErrorCode code;
  std::string message;
};

enum class ConsoleCheckerSpreadableErrorCode {
  NOT_AN_ARRAY,
};

struct ConsoleCheckerSpreadableError {
  ConsoleCheckerSpreadableErrorCode code;
  std::string message;
};

enum class ConsoleCheckerConsoleMethodErrorCode {
  NOT_SUPPORTED_YET,
};

struct ConsoleCheckerConsoleMethodError {
  ConsoleCheckerConsoleMethodErrorCode code;
  std::string message;
};

class ErrorTextConsole {
public:
  static std::string voidValue();
  static std::string spread(llvm::StringRef type);
  static std::string notYet(llvm::StringRef name);
};

}
