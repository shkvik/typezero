#pragma once

#include "llvm/ADT/StringRef.h"
#include <string>

namespace tz {

enum class CodeIndexingRegistryCheckIndexErrorCode {
  ALREADY_REPORTED,
  ENUM_STRING,
  ENUM_NOT_INTEGER,
  GENERIC_INDEX,
  UNKNOWN_NOT_NARROWED,
  KEY_REJECTED,
  FIELD_REJECTED,
  DICT_INDEX,
  VALUE_MAY_BE_UNDEFINED,
  NOT_INDEXABLE,
};

struct CodeIndexingRegistryCheckIndexError {
  CodeIndexingRegistryCheckIndexErrorCode code;
  std::string message;
};

enum class CodeIndexingRegistryPositionTypeErrorCode {
  NOT_AN_INTEGER,
};

struct CodeIndexingRegistryPositionTypeError {
  CodeIndexingRegistryPositionTypeErrorCode code;
  std::string message;
};

enum class CodeIndexingRegistryCheckGenericIndexErrorCode {
  NOT_A_GENERIC_INDEX,
  NOT_INDEXED_BY_KEY,
};

struct CodeIndexingRegistryCheckGenericIndexError {
  CodeIndexingRegistryCheckGenericIndexErrorCode code;
  std::string message;
};

enum class CodeIndexingRegistryCheckEnumIndexErrorCode {
  ALREADY_REPORTED,
  STRING_ENUM,
  NOT_AN_INTEGER,
};

struct CodeIndexingRegistryCheckEnumIndexError {
  CodeIndexingRegistryCheckEnumIndexErrorCode code;
  std::string message;
};

class ErrorTextIndexRead {
public:
  static std::string undefined();
  static std::string notIndexed(llvm::StringRef type);
};

class ErrorTextPosition {
public:
  static std::string notInt(llvm::StringRef type, bool fraction, bool undefined);
};

class ErrorTextGenericIndex {
public:
  static std::string notByKey(llvm::StringRef object, llvm::StringRef key);
};

class ErrorTextEnumIndex {
public:
  static std::string strings(llvm::StringRef name);
  static std::string notInteger(llvm::StringRef type);
};

}
