#pragma once

#include "llvm/ADT/StringRef.h"
#include <string>

namespace tz {

enum class CodeArithmeticRegistryCheckBitsErrorCode {
  NOTHING_CHANGES,
  DOUBLE_OPERAND,
  OPERAND_TYPES,
  SIGNED_SHIFT,
  MIXED_NUMBERS,
  DIVISOR_ZERO,
};

struct CodeArithmeticRegistryCheckBitsError {
  CodeArithmeticRegistryCheckBitsErrorCode code;
  std::string message;
};

enum class CodeArithmeticRegistryCheckWrappingErrorCode {
  MIXED_NUMBERS,
  DIVISOR_ZERO,
};

struct CodeArithmeticRegistryCheckWrappingError {
  CodeArithmeticRegistryCheckWrappingErrorCode code;
  std::string message;
};

enum class CodeArithmeticRegistryPowerExponentErrorCode {
  NEGATIVE_POWER,
};

struct CodeArithmeticRegistryPowerExponentError {
  CodeArithmeticRegistryPowerExponentErrorCode code;
  std::string message;
};

enum class CodeArithmeticRegistrySignOperandErrorCode {
  STRING_OR_BOOL,
};

struct CodeArithmeticRegistrySignOperandError {
  CodeArithmeticRegistrySignOperandErrorCode code;
  std::string message;
};

enum class CodeArithmeticRegistryNumberOperandErrorCode {
  DECIMAL_PLUS,
  NOT_NUMERIC,
};

struct CodeArithmeticRegistryNumberOperandError {
  CodeArithmeticRegistryNumberOperandErrorCode code;
  std::string message;
};

enum class CodeArithmeticRegistryBitsOperandErrorCode {
  DOUBLE_BITS,
  NOT_INTEGER,
};

struct CodeArithmeticRegistryBitsOperandError {
  CodeArithmeticRegistryBitsOperandErrorCode code;
  std::string message;
};

enum class CodeArithmeticRegistryCheckDivisorErrorCode {
  DIVISION_BY_ZERO,
};

struct CodeArithmeticRegistryCheckDivisorError {
  CodeArithmeticRegistryCheckDivisorErrorCode code;
  std::string message;
};

enum class CodeArithmeticRegistryShiftCountErrorCode {
  OUTSIDE_BITS,
};

struct CodeArithmeticRegistryShiftCountError {
  CodeArithmeticRegistryShiftCountErrorCode code;
  std::string message;
};

class ErrorTextMixedNumbers {
public:
  static std::string mixed(llvm::StringRef op, llvm::StringRef lhs, llvm::StringRef rhs,
                           llvm::StringRef example);
  static std::string example(llvm::StringRef left, llvm::StringRef op, llvm::StringRef right);
  static std::string method(llvm::StringRef method);
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

class ErrorTextPower {
public:
  static std::string negative(llvm::StringRef type);
};

class ErrorTextDivisor {
public:
  static std::string zero();
};

class ErrorTextShiftCount {
public:
  static std::string outside(int64_t count, int last, llvm::StringRef type);
};

class ErrorTextOverflow {
public:
  static std::string inInt(llvm::StringRef value, llvm::StringRef type, int64_t wrapped);
  static std::string inDouble(llvm::StringRef value, llvm::StringRef type, int64_t wrapped);
};

}
