#pragma once

#include "llvm/ADT/StringRef.h"
#include <string>

namespace tz {

enum class KeyedFieldCheckerCheckKeyedFieldErrorCode {
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

struct KeyedFieldCheckerCheckKeyedFieldError {
  KeyedFieldCheckerCheckKeyedFieldErrorCode code;
  std::string message;
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
