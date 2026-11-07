#pragma once

#include "llvm/ADT/StringRef.h"
#include <string>

namespace tz {

enum class NumberMethodsCheckStaticCallErrorCode {
  RAW_NOT_TAGGED,
  ALREADY_REPORTED,
  PARSE_FAILED,
  WRONG_ARITY,
  NO_SUCH_PROPERTY,
};

struct NumberMethodsCheckStaticCallError {
  NumberMethodsCheckStaticCallErrorCode code;
  std::string message;
};

class ErrorTextNumberStatic {
public:
  static std::string rawTag();
  static std::string noProperty(llvm::StringRef name, llvm::StringRef owner);
};

enum class NumberMethodsCheckConversionErrorCode {
  NOT_A_CONVERSION,
  WRONG_ARITY,
  DOUBLE_TO_DECIMAL,
  DOUBLE_TO_INTEGER,
};

struct NumberMethodsCheckConversionError {
  NumberMethodsCheckConversionErrorCode code;
  std::string message;
};

class ErrorTextConversion {
public:
  static std::string toDecimal(llvm::StringRef value);
  static std::string toInteger(llvm::StringRef value, llvm::StringRef method);
};

enum class NumberMethodsCheckNumberMethodErrorCode {
  CONVERSION_FAILED,
  WRONG_ARITY,
  NO_SUCH_PROPERTY,
};

struct NumberMethodsCheckNumberMethodError {
  NumberMethodsCheckNumberMethodErrorCode code;
  std::string message;
};

class ErrorTextNumberMethod {
public:
  static std::string noProperty(llvm::StringRef name, llvm::StringRef type);
  static std::string codePoint();
  static std::string radix();
  static std::string digits();
};

}
