#pragma once

#include "llvm/ADT/StringRef.h"
#include <string>

namespace tz {

enum class AssignCheckerNotImportedErrorCode {
  IMPORTED,
};

struct AssignCheckerNotImportedError {
  AssignCheckerNotImportedErrorCode code;
  std::string message;
};

class ErrorTextAssign {
public:
  static std::string imported(llvm::StringRef name);
  static std::string nullishVar();
  static std::string notOptional(llvm::StringRef name, llvm::StringRef type);
  static std::string indexRange();
  static std::string untyped(llvm::StringRef name);
  static std::string outOfRange();
  static std::string lengthRange(llvm::StringRef element, llvm::StringRef array);
  static std::string length();
};

enum class AssignCheckerVariableTargetErrorCode {
  NOT_A_VARIABLE,
};

struct AssignCheckerVariableTargetError {
  AssignCheckerVariableTargetErrorCode code;
  std::string message;
};

enum class AssignCheckerNullishOptionalErrorCode {
  NOT_OPTIONAL,
};

struct AssignCheckerNullishOptionalError {
  AssignCheckerNullishOptionalErrorCode code;
  std::string message;
};

enum class AssignCheckerIndexProvenErrorCode {
  INDEX_NOT_PROVEN,
};

struct AssignCheckerIndexProvenError {
  AssignCheckerIndexProvenErrorCode code;
  std::string message;
};

enum class AssignCheckerFieldTypeFromErrorCode {
  UNTYPED_VALUE,
};

struct AssignCheckerFieldTypeFromError {
  AssignCheckerFieldTypeFromErrorCode code;
  std::string message;
};

enum class AssignCheckerStringAssignErrorCode {
  OPERAND_TYPES,
  JOINS_AS_TEXT,
};

struct AssignCheckerStringAssignError {
  AssignCheckerStringAssignErrorCode code;
  std::string message;
};

class ErrorTextCompound {
public:
  static std::string joinsText(llvm::StringRef value, llvm::StringRef hint);
  static std::string convertTo();
  static std::string writeAs(llvm::StringRef target, llvm::StringRef value);
  static std::string bitsDouble(llvm::StringRef spelling);
  static std::string nothing(llvm::StringRef spelling);
  static std::string signedTo(llvm::StringRef type);
  static std::string mixed(llvm::StringRef spelling, llvm::StringRef type, llvm::StringRef value,
                           llvm::StringRef hint);
};

enum class AssignCheckerCompoundOperandsErrorCode {
  DOUBLE_BITS,
  OPERAND_TYPES,
  NOTHING_CHANGES,
  SIGNED_SHIFT,
  MIXED_NUMBERS,
};

struct AssignCheckerCompoundOperandsError {
  AssignCheckerCompoundOperandsErrorCode code;
  std::string message;
};

enum class AssignCheckerElementWritableErrorCode {
  OUT_OF_RANGE,
};

struct AssignCheckerElementWritableError {
  AssignCheckerElementWritableErrorCode code;
  std::string message;
};

enum class AssignCheckerLengthProvenErrorCode {
  LENGTH_NOT_PROVEN,
};

struct AssignCheckerLengthProvenError {
  AssignCheckerLengthProvenErrorCode code;
  std::string message;
};

enum class AssignCheckerCheckLogicalAssignErrorCode {
  NOT_A_SIMPLE_TARGET,
};

struct AssignCheckerCheckLogicalAssignError {
  AssignCheckerCheckLogicalAssignErrorCode code;
  std::string message;
};

class ErrorTextLogicalAssign {
public:
  static std::string simple(llvm::StringRef op);
  static std::string field();
  static std::string fieldOpt(llvm::StringRef name, llvm::StringRef type);
};

enum class AssignCheckerFieldChainErrorCode {
  NOT_A_FIELD_CHAIN,
};

struct AssignCheckerFieldChainError {
  AssignCheckerFieldChainErrorCode code;
  std::string message;
};

enum class AssignCheckerFieldOptionalErrorCode {
  NOT_OPTIONAL,
};

struct AssignCheckerFieldOptionalError {
  AssignCheckerFieldOptionalErrorCode code;
  std::string message;
};

enum class AssignCheckerUpdatableErrorCode {
  NOT_NUMERIC,
};

struct AssignCheckerUpdatableError {
  AssignCheckerUpdatableErrorCode code;
  std::string message;
};

class ErrorTextUpdate {
public:
  static std::string notNumber(llvm::StringRef op, llvm::StringRef type);
};

}
