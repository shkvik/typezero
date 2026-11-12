#pragma once

#include "llvm/ADT/StringRef.h"
#include <string>

namespace tz {

enum class ClassMemberCheckerCheckAccessErrorCode {
  PROTECTED_ELSEWHERE,
  PRIVATE_MEMBER,
  PROTECTED_MEMBER,
};

struct ClassMemberCheckerCheckAccessError {
  ClassMemberCheckerCheckAccessErrorCode code;
  std::string message;
};

class ErrorTextAccess {
public:
  static std::string elsewhere(llvm::StringRef name, llvm::StringRef cls, llvm::StringRef current,
                               llvm::StringRef other);
  static std::string hiddenPrivate(llvm::StringRef name, llvm::StringRef word, llvm::StringRef cls);
  static std::string hiddenProtected(llvm::StringRef name, llvm::StringRef word,
                                     llvm::StringRef cls);
};

enum class ClassMemberCheckerFieldSetErrorCode {
  READ_BEFORE_SET,
};

struct ClassMemberCheckerFieldSetError {
  ClassMemberCheckerFieldSetErrorCode code;
  std::string message;
};

class ErrorTextClassField {
public:
  static std::string readEarly(llvm::StringRef name);
  static std::string laterType(llvm::StringRef name);
  static std::string isStatic(llvm::StringRef name, llvm::StringRef cls);
  static std::string noProperty(llvm::StringRef name, llvm::StringRef type);
  static std::string earlyThis(llvm::StringRef name, llvm::StringRef field);
  static std::string genericVal(llvm::StringRef name);
};

enum class ClassMemberCheckerFieldTypedErrorCode {
  TYPE_FROM_INITIALIZER,
};

struct ClassMemberCheckerFieldTypedError {
  ClassMemberCheckerFieldTypedErrorCode code;
  std::string message;
};

enum class ClassMemberCheckerInstanceMethodsErrorCode {
  STATIC_MEMBER,
  NO_SUCH_PROPERTY,
};

struct ClassMemberCheckerInstanceMethodsError {
  ClassMemberCheckerInstanceMethodsErrorCode code;
  std::string message;
};

enum class ClassMemberCheckerObjectReadyErrorCode {
  USED_BEFORE_SET,
};

struct ClassMemberCheckerObjectReadyError {
  ClassMemberCheckerObjectReadyErrorCode code;
  std::string message;
};

enum class ClassMemberCheckerMethodValueErrorCode {
  GENERIC_METHOD,
};

struct ClassMemberCheckerMethodValueError {
  ClassMemberCheckerMethodValueErrorCode code;
  std::string message;
};

enum class ClassMemberCheckerStatsDateErrorCode {
  DERIVED_DATE,
};

struct ClassMemberCheckerStatsDateError {
  ClassMemberCheckerStatsDateErrorCode code;
  std::string message;
};

class ErrorTextFieldTarget {
public:
  static std::string statsDate(llvm::StringRef name);
  static std::string setter(llvm::StringRef name, llvm::StringRef type, llvm::StringRef setter);
  static std::string library(llvm::StringRef name, llvm::StringRef type);
  static std::string method(llvm::StringRef name, llvm::StringRef type);
  static std::string readOnly(llvm::StringRef name, llvm::StringRef type);
};

enum class ClassMemberCheckerAssignableFieldErrorCode {
  SETTER_METHOD,
  LIBRARY_METHOD,
  CLASS_METHOD,
  NO_SUCH_PROPERTY,
};

struct ClassMemberCheckerAssignableFieldError {
  ClassMemberCheckerAssignableFieldErrorCode code;
  std::string message;
};

enum class ClassMemberCheckerFieldWritableErrorCode {
  READ_ONLY,
};

struct ClassMemberCheckerFieldWritableError {
  ClassMemberCheckerFieldWritableErrorCode code;
  std::string message;
};

}
