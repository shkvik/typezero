#pragma once

#include "llvm/ADT/StringRef.h"
#include <string>

namespace tz {

enum class UnaryCheckerSignOperandErrorCode {
  STRING_OR_BOOL,
};

struct UnaryCheckerSignOperandError {
  UnaryCheckerSignOperandErrorCode code;
  std::string message;
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

enum class UnaryCheckerNumberOperandErrorCode {
  DECIMAL_PLUS,
  NOT_NUMERIC,
};

struct UnaryCheckerNumberOperandError {
  UnaryCheckerNumberOperandErrorCode code;
  std::string message;
};

enum class UnaryCheckerBitsOperandErrorCode {
  DOUBLE_BITS,
  NOT_INTEGER,
};

struct UnaryCheckerBitsOperandError {
  UnaryCheckerBitsOperandErrorCode code;
  std::string message;
};

enum class UnaryCheckerResultKeptErrorCode {
  DROPS_RESULT,
};

struct UnaryCheckerResultKeptError {
  UnaryCheckerResultKeptErrorCode code;
  std::string message;
};

enum class UnaryCheckerNonNullOperandErrorCode {
  ALWAYS_UNDEFINED,
  NEVER_UNDEFINED,
  NOT_SUPPORTED,
};

struct UnaryCheckerNonNullOperandError {
  UnaryCheckerNonNullOperandErrorCode code;
  std::string message;
};

enum class UnaryCheckerCheckDeleteErrorCode {
  OPTIONAL_FIELD,
  REQUIRED_FIELD,
  NOT_A_FIELD,
  DICT_DELETE,
  ARRAY_HOLE,
  STRING_CHARACTER,
  TUPLE_HOLE,
  OBJECT_FIELD,
};

struct UnaryCheckerCheckDeleteError {
  UnaryCheckerCheckDeleteErrorCode code;
  std::string message;
};

class ErrorTextDelete {
public:
  static std::string optional(llvm::StringRef name, llvm::StringRef text);
  static std::string required(llvm::StringRef name);
  static std::string notField();
  static std::string hole(llvm::StringRef array, llvm::StringRef position);
  static std::string character();
  static std::string tupleHole();
  static std::string objectFld();
};

enum class UnaryCheckerCheckInErrorCode {
  DICT_UNDEFINED,
  DICT_NUMBER_KEY,
  DICT_NOT_RECORD,
  UNION_UNDEFINED,
  UNION_NO_KEY,
  OPTIONAL_FIELD,
  ALWAYS_SAME,
  TUPLE_IN,
  OBJECT_OPTIONAL,
  OBJECT_ALWAYS_TRUE,
  OBJECT_ALWAYS_FALSE,
  OBJECT_IN,
  OPERAND_TYPES,
  ARRAY_IN,
};

struct UnaryCheckerCheckInError {
  UnaryCheckerCheckInErrorCode code;
  std::string message;
};

class ErrorTextIn {
public:
  static std::string undefined();
  static std::string byLiteral();
  static std::string maybeUndef(llvm::StringRef name, llvm::StringRef member, llvm::StringRef text);
  static std::string always(bool with, llvm::StringRef unionName, llvm::StringRef name);
  static std::string tuple();
  static std::string objMaybe(llvm::StringRef text);
  static std::string objTrue();
  static std::string objFalse(llvm::StringRef type, llvm::StringRef name);
  static std::string objPlain();
  static std::string array(llvm::StringRef check);
};

}
