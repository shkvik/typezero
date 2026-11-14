#pragma once

#include "llvm/ADT/StringRef.h"
#include <string>

namespace tz {

enum class CodeExpressionsRegistryCheckConditionalErrorCode {
  INCOMPATIBLE_BRANCHES,
};

struct CodeExpressionsRegistryCheckConditionalError {
  CodeExpressionsRegistryCheckConditionalErrorCode code;
  std::string message;
};

enum class CodeExpressionsRegistryCheckCastErrorCode {
  NOT_SUPPORTED,
  ARRAY_LITERAL,
  CONVERTS_BY_ITSELF,
  MAY_BE_UNDEFINED,
  NEEDS_CONVERSION,
  DOUBLE_TO_DECIMAL,
  DECIMAL_TO_NUMBER,
  JSON_VALUE,
  CANNOT_BECOME,
};

struct CodeExpressionsRegistryCheckCastError {
  CodeExpressionsRegistryCheckCastErrorCode code;
  std::string message;
};

class ErrorTextConditional {
public:
  static std::string mismatch(llvm::StringRef thenType, llvm::StringRef elseType);
};

class ErrorTextCast {
public:
  static std::string plain();
  static std::string declare(llvm::StringRef to);
  static std::string itself(llvm::StringRef to);
  static std::string undefined();
  static std::string hinted(llvm::StringRef hint);
  static std::string toDecimal();
  static std::string toNumber();
  static std::string json();
  static std::string cannot(llvm::StringRef from, llvm::StringRef to);
};

}
