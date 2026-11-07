#pragma once

#include "llvm/ADT/StringRef.h"
#include <string>

namespace tz {

class ErrorTextOperands {
public:
  static std::string types(llvm::StringRef op, llvm::StringRef lhs, llvm::StringRef rhs,
                           bool undefined);
};

class ErrorTextMixedNumbers {
public:
  static std::string mixed(llvm::StringRef op, llvm::StringRef lhs, llvm::StringRef rhs,
                           llvm::StringRef example);
  static std::string example(llvm::StringRef left, llvm::StringRef op, llvm::StringRef right);
  static std::string method(llvm::StringRef method);
};

enum class BinaryCheckerCheckNullishErrorCode {
  VOID_OPERAND,
  NO_COMMON_TYPE,
};

struct BinaryCheckerCheckNullishError {
  BinaryCheckerCheckNullishErrorCode code;
  std::string message;
};

enum class BinaryCheckerCheckTypeOfNameErrorCode {
  ALREADY_REPORTED,
  NO_NUMBER,
  NO_SYMBOL,
  NOT_A_NAME,
  NEVER_MATCHES,
};

struct BinaryCheckerCheckTypeOfNameError {
  BinaryCheckerCheckTypeOfNameErrorCode code;
  std::string message;
};

class ErrorTextTypeOfName {
public:
  static std::string noNumber(llvm::StringRef here);
  static std::string forType(llvm::StringRef type, llvm::StringRef names);
  static std::string noSymbol(llvm::StringRef names);
  static std::string notName(llvm::StringRef name, llvm::StringRef names);
  static std::string never(llvm::StringRef type, llvm::StringRef names, llvm::StringRef name);
  static std::string bigint();
};

enum class BinaryCheckerCheckBitsErrorCode {
  NOTHING_CHANGES,
  DOUBLE_OPERAND,
  OPERAND_TYPES,
  SIGNED_SHIFT,
  MIXED_NUMBERS,
  DIVISOR_ZERO,
};

struct BinaryCheckerCheckBitsError {
  BinaryCheckerCheckBitsErrorCode code;
  std::string message;
};

class ErrorTextNoOpBits {
public:
  static std::string nothing(llvm::StringRef lhs, llvm::StringRef op, llvm::StringRef hint);
  static std::string toInt(llvm::StringRef lhs);
  static std::string trunc(llvm::StringRef lhs);
  static std::string wrap(llvm::StringRef text, bool unsignedShift);
};

class ErrorTextBitsDouble {
public:
  static std::string fraction(llvm::StringRef op, llvm::StringRef side);
};

class ErrorTextShift {
public:
  static std::string isSigned(llvm::StringRef type, llvm::StringRef text, llvm::StringRef method,
                              llvm::StringRef count);
};

enum class BinaryCheckerCheckWrappingErrorCode {
  MIXED_NUMBERS,
  DIVISOR_ZERO,
};

struct BinaryCheckerCheckWrappingError {
  BinaryCheckerCheckWrappingErrorCode code;
  std::string message;
};

enum class BinaryCheckerPowerExponentErrorCode {
  NEGATIVE_POWER,
};

struct BinaryCheckerPowerExponentError {
  BinaryCheckerPowerExponentErrorCode code;
  std::string message;
};

class ErrorTextPower {
public:
  static std::string negative(llvm::StringRef type);
};

enum class BinaryCheckerNoResultsErrorCode {
  RESULTS_COMPARED,
};

struct BinaryCheckerNoResultsError {
  BinaryCheckerNoResultsErrorCode code;
  std::string message;
};

class ErrorTextResultEquality {
public:
  static std::string compared(llvm::StringRef op);
};

}
