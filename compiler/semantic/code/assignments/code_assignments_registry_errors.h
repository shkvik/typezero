#pragma once

#include "llvm/ADT/StringRef.h"
#include <string>

namespace tz {

enum class CodeAssignmentsRegistryNotImportedErrorCode {
  IMPORTED,
};

struct CodeAssignmentsRegistryNotImportedError {
  CodeAssignmentsRegistryNotImportedErrorCode code;
  std::string message;
};

enum class CodeAssignmentsRegistryVariableTargetErrorCode {
  NOT_A_VARIABLE,
};

struct CodeAssignmentsRegistryVariableTargetError {
  CodeAssignmentsRegistryVariableTargetErrorCode code;
  std::string message;
};

enum class CodeAssignmentsRegistryNullishOptionalErrorCode {
  NOT_OPTIONAL,
};

struct CodeAssignmentsRegistryNullishOptionalError {
  CodeAssignmentsRegistryNullishOptionalErrorCode code;
  std::string message;
};

enum class CodeAssignmentsRegistryIndexProvenErrorCode {
  INDEX_NOT_PROVEN,
};

struct CodeAssignmentsRegistryIndexProvenError {
  CodeAssignmentsRegistryIndexProvenErrorCode code;
  std::string message;
};

enum class CodeAssignmentsRegistryFieldTypeFromErrorCode {
  UNTYPED_VALUE,
};

struct CodeAssignmentsRegistryFieldTypeFromError {
  CodeAssignmentsRegistryFieldTypeFromErrorCode code;
  std::string message;
};

enum class CodeAssignmentsRegistryStringAssignErrorCode {
  OPERAND_TYPES,
  JOINS_AS_TEXT,
};

struct CodeAssignmentsRegistryStringAssignError {
  CodeAssignmentsRegistryStringAssignErrorCode code;
  std::string message;
};

enum class CodeAssignmentsRegistryCompoundOperandsErrorCode {
  DOUBLE_BITS,
  OPERAND_TYPES,
  NOTHING_CHANGES,
  SIGNED_SHIFT,
  MIXED_NUMBERS,
};

struct CodeAssignmentsRegistryCompoundOperandsError {
  CodeAssignmentsRegistryCompoundOperandsErrorCode code;
  std::string message;
};

enum class CodeAssignmentsRegistryElementWritableErrorCode {
  OUT_OF_RANGE,
};

struct CodeAssignmentsRegistryElementWritableError {
  CodeAssignmentsRegistryElementWritableErrorCode code;
  std::string message;
};

enum class CodeAssignmentsRegistryLengthProvenErrorCode {
  LENGTH_NOT_PROVEN,
};

struct CodeAssignmentsRegistryLengthProvenError {
  CodeAssignmentsRegistryLengthProvenErrorCode code;
  std::string message;
};

enum class CodeAssignmentsRegistryCheckLogicalAssignErrorCode {
  NOT_A_SIMPLE_TARGET,
};

struct CodeAssignmentsRegistryCheckLogicalAssignError {
  CodeAssignmentsRegistryCheckLogicalAssignErrorCode code;
  std::string message;
};

enum class CodeAssignmentsRegistryFieldChainErrorCode {
  NOT_A_FIELD_CHAIN,
};

struct CodeAssignmentsRegistryFieldChainError {
  CodeAssignmentsRegistryFieldChainErrorCode code;
  std::string message;
};

enum class CodeAssignmentsRegistryFieldOptionalErrorCode {
  NOT_OPTIONAL,
};

struct CodeAssignmentsRegistryFieldOptionalError {
  CodeAssignmentsRegistryFieldOptionalErrorCode code;
  std::string message;
};

enum class CodeAssignmentsRegistryUpdatableErrorCode {
  NOT_NUMERIC,
};

struct CodeAssignmentsRegistryUpdatableError {
  CodeAssignmentsRegistryUpdatableErrorCode code;
  std::string message;
};

enum class CodeAssignmentsRegistryIndexNotBytesErrorCode {
  BYTES_INDEXED,
};

struct CodeAssignmentsRegistryIndexNotBytesError {
  CodeAssignmentsRegistryIndexNotBytesErrorCode code;
  std::string message;
};

enum class CodeAssignmentsRegistryObjectDefinedErrorCode {
  MAY_BE_UNDEFINED,
};

struct CodeAssignmentsRegistryObjectDefinedError {
  CodeAssignmentsRegistryObjectDefinedErrorCode code;
  std::string message;
};

enum class CodeAssignmentsRegistryIndexWritableErrorCode {
  STRING_IMMUTABLE,
  ARRAY_MAY_BE_UNDEFINED,
  NOT_INDEXABLE,
};

struct CodeAssignmentsRegistryIndexWritableError {
  CodeAssignmentsRegistryIndexWritableErrorCode code;
  std::string message;
};

enum class CodeAssignmentsRegistryDictDefinedErrorCode {
  MAY_BE_UNDEFINED,
};

struct CodeAssignmentsRegistryDictDefinedError {
  CodeAssignmentsRegistryDictDefinedErrorCode code;
  std::string message;
};

enum class CodeAssignmentsRegistryBytesMemberErrorCode {
  LENGTH_FIXED,
  NOT_WRITABLE,
};

struct CodeAssignmentsRegistryBytesMemberError {
  CodeAssignmentsRegistryBytesMemberErrorCode code;
  std::string message;
};

enum class CodeAssignmentsRegistryMemberWritableErrorCode {
  COMPOUND_LENGTH,
  STRING_LENGTH,
  NOT_WRITABLE,
};

struct CodeAssignmentsRegistryMemberWritableError {
  CodeAssignmentsRegistryMemberWritableErrorCode code;
  std::string message;
};

enum class CodeAssignmentsRegistryNotThisErrorCode {
  ASSIGN_TO_THIS,
};

struct CodeAssignmentsRegistryNotThisError {
  CodeAssignmentsRegistryNotThisErrorCode code;
  std::string message;
};

enum class CodeAssignmentsRegistryVariableMutableErrorCode {
  CONSTANT,
};

struct CodeAssignmentsRegistryVariableMutableError {
  CodeAssignmentsRegistryVariableMutableErrorCode code;
  std::string message;
};

enum class CodeAssignmentsRegistryCheckDeleteErrorCode {
  OPTIONAL_FIELD,
  REQUIRED_FIELD,
  NOT_A_FIELD,
  DICT_DELETE,
  ARRAY_HOLE,
  STRING_CHARACTER,
  TUPLE_HOLE,
  OBJECT_FIELD,
};

struct CodeAssignmentsRegistryCheckDeleteError {
  CodeAssignmentsRegistryCheckDeleteErrorCode code;
  std::string message;
};

enum class CodeAssignmentsRegistryCheckImportAssignErrorCode {
  IMPORTED_VARIABLE,
};

struct CodeAssignmentsRegistryCheckImportAssignError {
  CodeAssignmentsRegistryCheckImportAssignErrorCode code;
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

class ErrorTextLogicalAssign {
public:
  static std::string simple(llvm::StringRef op);
  static std::string field();
  static std::string fieldOpt(llvm::StringRef name, llvm::StringRef type);
};

class ErrorTextUpdate {
public:
  static std::string notNumber(llvm::StringRef op, llvm::StringRef type);
};

class ErrorTextIndexTarget {
public:
  static std::string bytes(llvm::StringRef type, bool dataView);
  static std::string undefined();
  static std::string immutable();
  static std::string arrayUndef();
  static std::string notIndexed(llvm::StringRef type);
};

class ErrorTextMemberTarget {
public:
  static std::string undefined(llvm::StringRef name);
  static std::string fixedSize(llvm::StringRef type);
  static std::string noAssign(llvm::StringRef name, llvm::StringRef type);
  static std::string onlyAssign();
  static std::string textLength();
};

class ErrorTextAssignTarget {
public:
  static std::string arrayForm();
  static std::string objectForm();
  static std::string notTarget();
};

class ErrorTextVarTarget {
public:
  static std::string toThis();
  static std::string constant(llvm::StringRef name, bool function);
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

class ErrorTextImportAssign {
public:
  static std::string imported(llvm::StringRef name);
};

}
