#pragma once

#include "llvm/ADT/StringRef.h"
#include <string>

namespace tz {

enum class LibraryConsoleRegistryPrintableErrorCode {
  VOID_VALUE,
};

struct LibraryConsoleRegistryPrintableError {
  LibraryConsoleRegistryPrintableErrorCode code;
  std::string message;
};

enum class LibraryConsoleRegistrySpreadableErrorCode {
  NOT_AN_ARRAY,
};

struct LibraryConsoleRegistrySpreadableError {
  LibraryConsoleRegistrySpreadableErrorCode code;
  std::string message;
};

enum class LibraryConsoleRegistryConsoleMethodErrorCode {
  NOT_SUPPORTED_YET,
};

struct LibraryConsoleRegistryConsoleMethodError {
  LibraryConsoleRegistryConsoleMethodErrorCode code;
  std::string message;
};

class ErrorTextConsole {
public:
  static std::string voidValue();
  static std::string spread(llvm::StringRef type);
  static std::string notYet(llvm::StringRef name);
};

}
