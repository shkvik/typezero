#pragma once

#include "compiler/library/intl/intl_check_helpers/intl_check_helpers.h"
#include "llvm/ADT/StringRef.h"
#include <string>

namespace tz {

enum class IntlOptionsCheckerOptionsGivenErrorCode {
  NO_DISPLAY_TYPE,
  NOT_AN_OBJECT,
};

struct IntlOptionsCheckerOptionsGivenError {
  IntlOptionsCheckerOptionsGivenErrorCode code;
  std::string message;
};

class ErrorTextIntlOptions {
public:
  static std::string noType();
  static std::string notObject(llvm::StringRef what, llvm::StringRef type);
};

class ErrorTextIntlField {
public:
  static std::string values(const std::vector<llvm::StringLiteral> &values);
  static std::string unknown(llvm::StringRef name, llvm::StringRef what,
                             const IntlService *service);
  static std::string kind(llvm::StringRef name, char kind, llvm::StringRef type);
  static std::string badValue(llvm::StringRef text, llvm::StringRef name,
                              const std::vector<llvm::StringLiteral> &values);
  static std::string open(llvm::StringRef name, const std::vector<llvm::StringLiteral> &values);
  static std::string currency(llvm::StringRef code);
  static std::string unit(llvm::StringRef unit);
  static std::string keyword(llvm::StringRef value, llvm::StringRef name);
  static std::string styleCur();
  static std::string styleUnit();
  static std::string range(llvm::StringRef name, int min, int max);
  static std::string rangeOpen(llvm::StringRef name, int min, int max);
  static std::string above(llvm::StringRef min, llvm::StringRef max, bool known);
  static std::string incSig();
  static std::string incSame();
  static std::string increments(bool known);
};

}
