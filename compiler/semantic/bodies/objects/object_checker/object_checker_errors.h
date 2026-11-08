#pragma once

#include "llvm/ADT/StringRef.h"
#include <string>

namespace tz {

enum class ObjectCheckerCheckObjectCallErrorCode {
  ALREADY_REPORTED,
  GROUP_BY_ARITY,
  GROUP_BY_KEY,
  FROM_ENTRIES_ARITY,
  FROM_ENTRIES_PAIRS,
  NOT_SUPPORTED,
  WRONG_ARITY,
  MAY_BE_UNDEFINED,
  DICT_ALWAYS_EMPTY,
  NOT_AN_OBJECT,
  EMITTER_FIELDS,
  METHOD_FIELDS,
  NO_COMMON_TYPE,
  NO_FIELDS,
  NUMERIC_ENUM,
};

struct ObjectCheckerCheckObjectCallError {
  ObjectCheckerCheckObjectCallErrorCode code;
  std::string message;
};

class ErrorTextObjectCall {
public:
  static std::string noSupport(llvm::StringRef fn);
  static std::string undefined();
  static std::string needObject(llvm::StringRef fn, llvm::StringRef type);
  static std::string emitter(llvm::StringRef fn);
  static std::string methods(llvm::StringRef fn);
  static std::string noUnion(llvm::StringRef fn, llvm::StringRef type, llvm::StringRef problem);
  static std::string noFields(llvm::StringRef type, llvm::StringRef fn);
  static std::string numberEnum(llvm::StringRef fn, llvm::StringRef name, llvm::StringRef member);
};

enum class ObjectCheckerEnumMembersErrorCode {
  NUMERIC_ENUM,
};

struct ObjectCheckerEnumMembersError {
  ObjectCheckerEnumMembersErrorCode code;
  std::string message;
};

enum class ObjectCheckerWalkableErrorCode {
  INDICES_AS_STRINGS,
  MAY_BE_UNDEFINED,
  DICT_PROPERTIES,
  NOT_AN_OBJECT,
};

struct ObjectCheckerWalkableError {
  ObjectCheckerWalkableErrorCode code;
  std::string message;
};

class ErrorTextForIn {
public:
  static std::string indices(llvm::StringRef what, llvm::StringRef text, bool array);
  static std::string undefined();
  static std::string dict(llvm::StringRef text, bool isMap);
  static std::string notObject(llvm::StringRef type);
  static std::string anArray();
  static std::string aString();
  static std::string aTuple();
};

enum class ObjectCheckerCheckFieldErrorCode {
  NO_SUCH_FIELD,
};

struct ObjectCheckerCheckFieldError {
  ObjectCheckerCheckFieldErrorCode code;
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

enum class ObjectCheckerCheckFieldCallErrorCode {
  NO_SUCH_FIELD,
  TAKES_THIS,
  MAY_BE_UNDEFINED,
  NOT_A_FUNCTION,
};

struct ObjectCheckerCheckFieldCallError {
  ObjectCheckerCheckFieldCallErrorCode code;
  std::string message;
};

enum class ObjectCheckerObjectDefinedErrorCode {
  MAY_BE_UNDEFINED,
};

struct ObjectCheckerObjectDefinedError {
  ObjectCheckerObjectDefinedErrorCode code;
  std::string message;
};

enum class ObjectCheckerFixedLengthErrorCode {
  TUPLE_LENGTH,
};

struct ObjectCheckerFixedLengthError {
  ObjectCheckerFixedLengthErrorCode code;
  std::string message;
};

enum class ObjectCheckerFieldExistsErrorCode {
  OVERLOADED_METHOD,
  NO_SUCH_FIELD,
};

struct ObjectCheckerFieldExistsError {
  ObjectCheckerFieldExistsErrorCode code;
  std::string message;
};

enum class ObjectCheckerFieldWritableErrorCode {
  READ_ONLY,
};

struct ObjectCheckerFieldWritableError {
  ObjectCheckerFieldWritableErrorCode code;
  std::string message;
};

class ErrorTextMismatch {
public:
  static std::string extraField(llvm::StringRef name);
  static std::string noField(llvm::StringRef name);
  static std::string fieldType(llvm::StringRef name, llvm::StringRef here, llvm::StringRef there,
                               llvm::StringRef to);
  static std::string typeHint(llvm::StringRef to);
  static std::string readonly(llvm::StringRef name, llvm::StringRef to);
};

}
