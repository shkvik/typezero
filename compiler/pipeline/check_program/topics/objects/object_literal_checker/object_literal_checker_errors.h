#pragma once

#include "llvm/ADT/StringRef.h"
#include <string>

namespace tz {

enum class ObjectLiteralCheckerPropertyPlainErrorCode {
  DEFAULT_VALUE,
};

struct ObjectLiteralCheckerPropertyPlainError {
  ObjectLiteralCheckerPropertyPlainErrorCode code;
  std::string message;
};

class ErrorTextObjectLiteral {
public:
  static std::string defaulted(llvm::StringRef name);
  static std::string numberKey();
  static std::string runtimeKey();
  static std::string keyType(llvm::StringRef type);
  static std::string duplicate(llvm::StringRef name);
  static std::string inferFrom(llvm::StringRef type);
  static std::string keyTypes(llvm::StringRef name);
  static std::string keySpread(llvm::StringRef type);
  static std::string keyNoType(llvm::StringRef type);
  static std::string keyFields(llvm::StringRef type, llvm::StringRef target);
  static std::string keyedOpt(llvm::StringRef type, llvm::StringRef field);
  static std::string noField(llvm::StringRef target, llvm::StringRef name, bool stream);
  static std::string voidValue(llvm::StringRef what);
  static std::string undefOf(llvm::StringRef what, llvm::StringRef name);
  static std::string missing(llvm::StringRef name, llvm::StringRef target);
  static std::string overloads(llvm::StringRef target, llvm::StringRef name);
  static std::string keyedName(llvm::StringRef type);
  static std::string fieldName(llvm::StringRef name);
};

enum class ObjectLiteralCheckerComputedKeyErrorCode {
  NUMBER_KEY,
  RUNTIME_KEY,
  NOT_A_STRING,
};

struct ObjectLiteralCheckerComputedKeyError {
  ObjectLiteralCheckerComputedKeyErrorCode code;
  std::string message;
};

enum class ObjectLiteralCheckerUniqueNameErrorCode {
  DUPLICATE_FIELD,
};

struct ObjectLiteralCheckerUniqueNameError {
  ObjectLiteralCheckerUniqueNameErrorCode code;
  std::string message;
};

enum class ObjectLiteralCheckerInferredValueErrorCode {
  CANNOT_INFER,
};

struct ObjectLiteralCheckerInferredValueError {
  ObjectLiteralCheckerInferredValueErrorCode code;
  std::string message;
};

enum class ObjectLiteralCheckerSameKeyedTypeErrorCode {
  FIELD_TYPES_DIFFER,
};

struct ObjectLiteralCheckerSameKeyedTypeError {
  ObjectLiteralCheckerSameKeyedTypeErrorCode code;
  std::string message;
};

enum class ObjectLiteralCheckerKeyedPlacementErrorCode {
  KEY_WITH_SPREAD,
};

struct ObjectLiteralCheckerKeyedPlacementError {
  ObjectLiteralCheckerKeyedPlacementErrorCode code;
  std::string message;
};

enum class ObjectLiteralCheckerKeyedTargetErrorCode {
  NO_OBJECT_TYPE,
};

struct ObjectLiteralCheckerKeyedTargetError {
  ObjectLiteralCheckerKeyedTargetErrorCode code;
  std::string message;
};

enum class ObjectLiteralCheckerKeyedFieldErrorCode {
  FIELDS_DIFFER,
  NOT_OPTIONAL,
};

struct ObjectLiteralCheckerKeyedFieldError {
  ObjectLiteralCheckerKeyedFieldErrorCode code;
  std::string message;
};

enum class ObjectLiteralCheckerTargetFieldErrorCode {
  NO_SUCH_FIELD,
  OVERLOAD_NAME,
};

struct ObjectLiteralCheckerTargetFieldError {
  ObjectLiteralCheckerTargetFieldErrorCode code;
  std::string message;
};

enum class ObjectLiteralCheckerValueTypedErrorCode {
  VOID_VALUE,
  UNDEFINED_VALUE,
};

struct ObjectLiteralCheckerValueTypedError {
  ObjectLiteralCheckerValueTypedErrorCode code;
  std::string message;
};

enum class ObjectLiteralCheckerFieldGivenErrorCode {
  MISSING_FIELD,
};

struct ObjectLiteralCheckerFieldGivenError {
  ObjectLiteralCheckerFieldGivenErrorCode code;
  std::string message;
};

enum class ObjectLiteralCheckerSpreadDefinedErrorCode {
  MAY_BE_UNDEFINED,
};

struct ObjectLiteralCheckerSpreadDefinedError {
  ObjectLiteralCheckerSpreadDefinedErrorCode code;
  std::string message;
};

class ErrorTextSpreadLiteral {
public:
  static std::string undefined();
  static std::string notObject(llvm::StringRef type, bool indices);
  static std::string methods();
  static std::string twoTypes(llvm::StringRef name, llvm::StringRef before, llvm::StringRef type,
                              llvm::StringRef spread);
};

enum class ObjectLiteralCheckerSpreadObjectErrorCode {
  NOT_AN_OBJECT,
};

struct ObjectLiteralCheckerSpreadObjectError {
  ObjectLiteralCheckerSpreadObjectErrorCode code;
  std::string message;
};

enum class ObjectLiteralCheckerSpreadFieldsErrorCode {
  METHOD_FIELDS,
};

struct ObjectLiteralCheckerSpreadFieldsError {
  ObjectLiteralCheckerSpreadFieldsErrorCode code;
  std::string message;
};

enum class ObjectLiteralCheckerSpreadJoinErrorCode {
  TYPES_DIFFER,
};

struct ObjectLiteralCheckerSpreadJoinError {
  ObjectLiteralCheckerSpreadJoinErrorCode code;
  std::string message;
};

}
