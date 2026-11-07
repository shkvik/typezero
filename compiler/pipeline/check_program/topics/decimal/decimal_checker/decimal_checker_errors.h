#pragma once

#include "compiler/program/types/types.h"
#include "llvm/ADT/StringRef.h"
#include <string>

namespace tz {

class ErrorTextBigInt {
public:
  static std::string none(llvm::StringRef what);
  static std::string quoted(llvm::StringRef name);
};

enum class DecimalCheckerRoundingModeErrorCode {
  UNKNOWN_MODE,
};

struct DecimalCheckerRoundingModeError {
  DecimalCheckerRoundingModeErrorCode code;
  std::string message;
};

enum class DecimalCheckerLiteralRangeErrorCode {
  UNREADABLE,
  OUT_OF_RANGE,
};

struct DecimalCheckerLiteralRangeError {
  DecimalCheckerLiteralRangeErrorCode code;
  std::string message;
};

class ErrorTextDecimalLit {
public:
  static std::string unreadable();
  static std::string outOfRange(int64_t digits);
};

class ErrorTextDecimalMix {
public:
  static std::string integer(llvm::StringRef op, llvm::StringRef left, llvm::StringRef right,
                             llvm::StringRef lhs, llvm::StringRef rhs, bool leftDecimal);
  static std::string inexact(llvm::StringRef op, llvm::StringRef left, llvm::StringRef right,
                             llvm::StringRef lhs, llvm::StringRef rhs, bool leftDecimal,
                             bool example);
};

class ErrorTextDecimalPower {
public:
  static std::string exponent(const Type *found);
  static std::string divZero();
};

class ErrorTextDecimalOf {
public:
  static std::string notNumber(llvm::StringRef text);
  static std::string takes(llvm::StringRef type, bool optional);
};

class ErrorTextDecimalStatic {
public:
  static std::string noMember(llvm::StringRef name);
  static std::string spread(llvm::StringRef type);
  static std::string noValue(llvm::StringRef name);
};

class ErrorTextRoundingMode {
public:
  static std::string unknown(llvm::StringRef mode);
  static std::string notKnown();
};

class ErrorTextDecimalCount {
public:
  static std::string notInt(llvm::StringRef what, llvm::StringRef type, bool optional);
  static std::string places();
  static std::string digits();
  static std::string base();
  static std::string power();
};

class ErrorTextDecimalMethod {
public:
  static std::string noMember(llvm::StringRef name, llvm::StringRef use);
  static std::string places();
};

}
