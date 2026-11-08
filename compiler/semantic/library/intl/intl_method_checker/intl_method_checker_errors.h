#pragma once

#include "compiler/program/types/types.h"
#include "llvm/ADT/StringRef.h"
#include <string>

namespace tz {

enum class IntlMethodCheckerIntlNumberErrorCode {
  NOT_A_NUMBER,
};

struct IntlMethodCheckerIntlNumberError {
  IntlMethodCheckerIntlNumberErrorCode code;
  std::string message;
};

enum class IntlMethodCheckerClosedArgErrorCode {
  UNKNOWN_TEXT,
  NOT_ALLOWED,
};

struct IntlMethodCheckerClosedArgError {
  IntlMethodCheckerClosedArgErrorCode code;
  std::string message;
};

class ErrorTextIntlNumber {
public:
  static std::string notNumber(llvm::StringRef method, const Type *type);
};

class ErrorTextClosedArg {
public:
  static std::string unknown(llvm::StringRef what, llvm::StringRef first, llvm::StringRef second);
  static std::string notAllowed(llvm::StringRef text, llvm::StringRef what);
  static std::string unit();
  static std::string form();
};

class ErrorTextIntlMethod {
public:
  static std::string range(llvm::StringRef name);
  static std::string selectSpan();
  static std::string badUnit(llvm::StringRef unit);
  static std::string noMember(llvm::StringRef name, llvm::StringRef service);
  static std::string badForm(llvm::StringRef form);
};

}
