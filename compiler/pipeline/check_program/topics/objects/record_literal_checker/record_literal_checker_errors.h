#pragma once

#include "llvm/ADT/StringRef.h"
#include <string>

namespace tz {

enum class RecordLiteralCheckerPlainPropertyErrorCode {
  DEFAULT_IN_LITERAL,
  METHOD_IN_RECORD,
};

struct RecordLiteralCheckerPlainPropertyError {
  RecordLiteralCheckerPlainPropertyErrorCode code;
  std::string message;
};

class ErrorTextRecordProperty {
public:
  static std::string defaulted(llvm::StringRef name);
  static std::string method(llvm::StringRef name);
  static std::string keyLabel(llvm::StringRef name);
  static std::string valueLabel();
};

enum class RecordLiteralCheckerCheckRecordSpreadErrorCode {
  MAY_BE_UNDEFINED,
  KEYED_REST,
  OTHER_VALUES,
  FIELD_TYPE_DIFFERS,
  HAS_METHODS,
  NOT_SPREADABLE,
};

struct RecordLiteralCheckerCheckRecordSpreadError {
  RecordLiteralCheckerCheckRecordSpreadErrorCode code;
  std::string message;
};

class ErrorTextRecordSpread {
public:
  static std::string undefined();
  static std::string keyedRest();
  static std::string values(llvm::StringRef type, llvm::StringRef value);
  static std::string field(llvm::StringRef field, llvm::StringRef type, llvm::StringRef fieldType,
                           llvm::StringRef value);
  static std::string methods();
  static std::string notObject(llvm::StringRef type, bool collection);
};

enum class RecordLiteralCheckerValuesJoinErrorCode {
  VALUE_TYPES_DIFFER,
};

struct RecordLiteralCheckerValuesJoinError {
  RecordLiteralCheckerValuesJoinErrorCode code;
  std::string message;
};

class ErrorTextRecordValues {
public:
  static std::string differ(llvm::StringRef value, llvm::StringRef other);
};

enum class RecordLiteralCheckerValueKnownErrorCode {
  ONLY_UNDEFINED,
};

struct RecordLiteralCheckerValueKnownError {
  RecordLiteralCheckerValueKnownErrorCode code;
  std::string message;
};

class ErrorTextRecordValue {
public:
  static std::string undefined();
};

}
