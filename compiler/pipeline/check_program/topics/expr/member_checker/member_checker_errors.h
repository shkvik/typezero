#pragma once

#include "llvm/ADT/StringRef.h"
#include <string>

namespace tz {

enum class MemberCheckerCheckMemberErrorCode {
  ENUM_MEMBER,
  DECIMAL_FUNCTION,
  GLOBAL_THIS,
  INTL_VALUE,
  NUMBER_PROPERTY,
  GLOBAL_FUNCTION,
  SUPER_MEMBER,
  UNKNOWN_MEMBER,
  UNKNOWN_NOT_NARROWED,
  UNION_MEMBER,
  TUPLE_MEMBER,
  WEAK_REF_MEMBER,
  DICT_MEMBER,
  OBJECT_FIELD,
  MAY_BE_UNDEFINED,
  RAW_TEXTS,
  METHOD_NOT_CALLED,
  NO_SUCH_PROPERTY,
};

struct MemberCheckerCheckMemberError {
  MemberCheckerCheckMemberErrorCode code;
  std::string message;
};

class ErrorTextMemberRead {
public:
  static std::string decimal(llvm::StringRef name);
  static std::string crypto();
  static std::string globalThis(llvm::StringRef name);
  static std::string intl(llvm::StringRef name);
  static std::string numberProp(llvm::StringRef name);
  static std::string asValue(llvm::StringRef global, llvm::StringRef name);
  static std::string superField(llvm::StringRef name);
  static std::string weakRef(llvm::StringRef name);
  static std::string undefined(llvm::StringRef name);
  static std::string raw();
  static std::string mustCall(llvm::StringRef name, llvm::StringRef type);
  static std::string noProperty(llvm::StringRef name, llvm::StringRef type);
};

enum class MemberCheckerCheckIndexErrorCode {
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

struct MemberCheckerCheckIndexError {
  MemberCheckerCheckIndexErrorCode code;
  std::string message;
};

class ErrorTextIndexRead {
public:
  static std::string undefined();
  static std::string notIndexed(llvm::StringRef type);
};

enum class MemberCheckerPositionTypeErrorCode {
  NOT_AN_INTEGER,
};

struct MemberCheckerPositionTypeError {
  MemberCheckerPositionTypeErrorCode code;
  std::string message;
};

class ErrorTextPosition {
public:
  static std::string notInt(llvm::StringRef type, bool fraction, bool undefined);
};

}
