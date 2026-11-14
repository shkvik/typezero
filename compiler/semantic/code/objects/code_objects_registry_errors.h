#pragma once

#include "llvm/ADT/StringRef.h"
#include <string>

namespace tz {

enum class CodeObjectsRegistryCheckFieldErrorCode {
  NO_SUCH_FIELD,
};

struct CodeObjectsRegistryCheckFieldError {
  CodeObjectsRegistryCheckFieldErrorCode code;
  std::string message;
};

enum class CodeObjectsRegistryCheckFieldCallErrorCode {
  NO_SUCH_FIELD,
  TAKES_THIS,
  MAY_BE_UNDEFINED,
  NOT_A_FUNCTION,
};

struct CodeObjectsRegistryCheckFieldCallError {
  CodeObjectsRegistryCheckFieldCallErrorCode code;
  std::string message;
};

enum class CodeObjectsRegistryObjectDefinedErrorCode {
  MAY_BE_UNDEFINED,
};

struct CodeObjectsRegistryObjectDefinedError {
  CodeObjectsRegistryObjectDefinedErrorCode code;
  std::string message;
};

enum class CodeObjectsRegistryFixedLengthErrorCode {
  TUPLE_LENGTH,
};

struct CodeObjectsRegistryFixedLengthError {
  CodeObjectsRegistryFixedLengthErrorCode code;
  std::string message;
};

enum class CodeObjectsRegistryFieldExistsErrorCode {
  OVERLOADED_METHOD,
  NO_SUCH_FIELD,
};

struct CodeObjectsRegistryFieldExistsError {
  CodeObjectsRegistryFieldExistsErrorCode code;
  std::string message;
};

enum class CodeObjectsRegistryFieldWritableErrorCode {
  READ_ONLY,
};

struct CodeObjectsRegistryFieldWritableError {
  CodeObjectsRegistryFieldWritableErrorCode code;
  std::string message;
};

enum class CodeObjectsRegistryCheckKeyedFieldErrorCode {
  ALREADY_REPORTED,
  TUPLE_KEY_NOT_NUMBER,
  TUPLE_KEY_NOT_CONSTANT,
  TUPLE_KEY_OUT_OF_RANGE,
  READONLY_THROUGH_KEY,
  ASSIGNED_BY_NAME,
  NO_NUMBERED_FIELDS,
  KEY_NOT_CONSTANT,
  KEY_NOT_STRING,
  NO_SUCH_FIELD,
  FIELD_NOT_FOUND,
  TUPLE_ELEMENT_BY_NUMBER,
  FIELD_KEY_NOT_LITERAL,
};

struct CodeObjectsRegistryCheckKeyedFieldError {
  CodeObjectsRegistryCheckKeyedFieldErrorCode code;
  std::string message;
};

class ErrorTextObjectField {
public:
  static std::string noField(llvm::StringRef name, llvm::StringRef type, bool inherited);
  static std::string maybeUndef();
  static std::string notFunc(llvm::StringRef name, llvm::StringRef type);
  static std::string setUndef(llvm::StringRef name);
  static std::string tupleLen();
  static std::string overloaded(llvm::StringRef name, llvm::StringRef type);
  static std::string readOnly(llvm::StringRef name, llvm::StringRef type);
};

class ErrorTextKeyedField {
public:
  static std::string tupleKey(llvm::StringRef key);
  static std::string tupleConst();
  static std::string tupleRange(llvm::StringRef tuple, size_t count, int64_t position);
  static std::string readonly();
  static std::string byName(llvm::StringRef field);
  static std::string numbered(llvm::StringRef type);
  static std::string runtimeKey(llvm::StringRef type);
  static std::string keyString(llvm::StringRef key);
  static std::string noField(llvm::StringRef name, llvm::StringRef type, bool inherited);
  static std::string tupleSet();
  static std::string keySet();
};

}
