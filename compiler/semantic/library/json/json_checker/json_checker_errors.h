#pragma once

#include "llvm/ADT/StringRef.h"
#include <string>

namespace tz {

enum class JsonCheckerTypeOfAgreesErrorCode {
  NEVER_NULL,
  NEVER_ARRAY,
};

struct JsonCheckerTypeOfAgreesError {
  JsonCheckerTypeOfAgreesErrorCode code;
  std::string message;
};

enum class JsonCheckerNullPossibleErrorCode {
  NEVER_NULL,
};

struct JsonCheckerNullPossibleError {
  JsonCheckerNullPossibleErrorCode code;
  std::string message;
};

enum class JsonCheckerTestsUnknownErrorCode {
  KNOWN_TYPE,
};

struct JsonCheckerTestsUnknownError {
  JsonCheckerTestsUnknownErrorCode code;
  std::string message;
};

class ErrorTextJsonTest {
public:
  static std::string neverNull(llvm::StringRef x, bool isAnd, llvm::StringRef name);
  static std::string neverArray(llvm::StringRef x, bool isAnd, llvm::StringRef name);
};

class ErrorTextJsonKey {
public:
  static std::string index(llvm::StringRef type);
  static std::string literal(llvm::StringRef type);
  static std::string label(llvm::StringRef name);
};

class ErrorTextJsonMethod {
public:
  static std::string none(llvm::StringRef name, llvm::StringRef variable);
};

class ErrorTextJsonLiteral {
public:
  static std::string spread();
  static std::string function();
  static std::string defaulted(llvm::StringRef name);
};

class ErrorTextJsonNull {
public:
  static std::string never(llvm::StringRef type, llvm::StringRef op);
};

class ErrorTextJsonIsJson {
public:
  static std::string known(llvm::StringRef type, bool json);
};

}
