#pragma once

#include "llvm/ADT/StringRef.h"
#include <string>

namespace tz {

enum class CodeOperatorsRegistryResultKeptErrorCode {
  DROPS_RESULT,
};

struct CodeOperatorsRegistryResultKeptError {
  CodeOperatorsRegistryResultKeptErrorCode code;
  std::string message;
};

enum class CodeOperatorsRegistryNonNullOperandErrorCode {
  ALWAYS_UNDEFINED,
  NEVER_UNDEFINED,
  NOT_SUPPORTED,
};

struct CodeOperatorsRegistryNonNullOperandError {
  CodeOperatorsRegistryNonNullOperandErrorCode code;
  std::string message;
};

class ErrorTextOperands {
public:
  static std::string types(llvm::StringRef op, llvm::StringRef lhs, llvm::StringRef rhs,
                           bool undefined);
};

class ErrorTextUnary {
public:
  static std::string noNumber(llvm::StringRef op, bool string);
  static std::string decimalSum();
  static std::string notNumber(llvm::StringRef op, llvm::StringRef type);
  static std::string bitsDouble();
  static std::string bitsType(llvm::StringRef type);
  static std::string dropsAsync();
  static std::string drops();
  static std::string allUndef();
  static std::string neverUndef(llvm::StringRef text);
  static std::string checkFirst(llvm::StringRef text);
};

}
