#pragma once

#include "llvm/ADT/StringRef.h"
#include <string>

namespace tz {

enum class ClassBasesEventMapFoundErrorCode {
  NO_EVENT_MAP,
};

struct ClassBasesEventMapFoundError {
  ClassBasesEventMapFoundErrorCode code;
  std::string message;
};

class ErrorTextExtends {
public:
  static std::string eventMap();
  static std::string byName();
  static std::string itself(llvm::StringRef cls, llvm::StringRef through);
  static std::string runtime(llvm::StringRef cls, llvm::StringRef parent);
  static std::string valueBase(llvm::StringRef cls, llvm::StringRef parent);
  static std::string errorClass(llvm::StringRef name, llvm::StringRef cls);
  static std::string notClass(llvm::StringRef name, llvm::StringRef cls, bool alias);
  static std::string noClass(llvm::StringRef name);
  static std::string classIface(llvm::StringRef type, bool extends);
  static std::string notIface(llvm::StringRef type);
};

enum class ClassBasesBaseClassErrorCode {
  NOT_NAMED,
  ALREADY_REPORTED,
  EXTENDS_ITSELF,
  RUNTIME_CLASS,
  VALUE_BASE,
  ERROR_CLASS,
  NOT_A_CLASS,
  NO_SUCH_CLASS,
};

struct ClassBasesBaseClassError {
  ClassBasesBaseClassErrorCode code;
  std::string message;
};

enum class ClassBasesImplementedTypeErrorCode {
  CLASS_AS_INTERFACE,
  NOT_AN_INTERFACE,
};

struct ClassBasesImplementedTypeError {
  ClassBasesImplementedTypeErrorCode code;
  std::string message;
};

enum class ClassBasesOverrideTargetErrorCode {
  NO_PARENT,
  NO_FIELD,
};

struct ClassBasesOverrideTargetError {
  ClassBasesOverrideTargetErrorCode code;
  std::string message;
};

class ErrorTextRedeclared {
public:
  static std::string noField(llvm::StringRef name, llvm::StringRef parent);
  static std::string noParent(llvm::StringRef name, llvm::StringRef cls);
  static std::string isPrivate(llvm::StringRef name, llvm::StringRef owner);
  static std::string narrower(llvm::StringRef name, llvm::StringRef baseAccess,
                              llvm::StringRef owner, llvm::StringRef access, llvm::StringRef cls);
  static std::string noValue(llvm::StringRef name, llvm::StringRef owner);
};

enum class ClassBasesInheritedVisibleErrorCode {
  PRIVATE_IN_BASE,
};

struct ClassBasesInheritedVisibleError {
  ClassBasesInheritedVisibleErrorCode code;
  std::string message;
};

enum class ClassBasesAccessKeptErrorCode {
  NARROWER_ACCESS,
};

struct ClassBasesAccessKeptError {
  ClassBasesAccessKeptErrorCode code;
  std::string message;
};

enum class ClassBasesValueGivenErrorCode {
  NO_VALUE,
};

struct ClassBasesValueGivenError {
  ClassBasesValueGivenErrorCode code;
  std::string message;
};

enum class ClassBasesNotSelfErrorCode {
  EXTENDS_ITSELF,
};

struct ClassBasesNotSelfError {
  ClassBasesNotSelfErrorCode code;
  std::string message;
};

class ErrorTextInterfaceBases {
public:
  static std::string itself(llvm::StringRef alias, llvm::StringRef through);
  static std::string classBase(llvm::StringRef type);
  static std::string notIface(llvm::StringRef type);
  static std::string twoMethods(llvm::StringRef alias, llvm::StringRef name);
  static std::string twoFields(llvm::StringRef alias, llvm::StringRef name, llvm::StringRef origin,
                               llvm::StringRef type, llvm::StringRef base, llvm::StringRef other);
  static std::string method(llvm::StringRef alias, llvm::StringRef name);
  static std::string member(llvm::StringRef alias, llvm::StringRef name);
  static std::string narrowed(llvm::StringRef name, llvm::StringRef alias, llvm::StringRef type,
                              llvm::StringRef origin, llvm::StringRef other);
};

enum class ClassBasesInterfaceBaseErrorCode {
  CLASS_BASE,
  NOT_AN_INTERFACE,
};

struct ClassBasesInterfaceBaseError {
  ClassBasesInterfaceBaseErrorCode code;
  std::string message;
};

enum class ClassBasesSameSignaturesErrorCode {
  OVERLOAD_CONFLICT,
};

struct ClassBasesSameSignaturesError {
  ClassBasesSameSignaturesErrorCode code;
  std::string message;
};

enum class ClassBasesSameFieldTypeErrorCode {
  FIELD_CONFLICT,
};

struct ClassBasesSameFieldTypeError {
  ClassBasesSameFieldTypeErrorCode code;
  std::string message;
};

enum class ClassBasesNewOverloadErrorCode {
  REDECLARED_METHOD,
};

struct ClassBasesNewOverloadError {
  ClassBasesNewOverloadErrorCode code;
  std::string message;
};

enum class ClassBasesNewFieldErrorCode {
  REDECLARED_MEMBER,
};

struct ClassBasesNewFieldError {
  ClassBasesNewFieldErrorCode code;
  std::string message;
};

enum class ClassBasesNarrowsFieldErrorCode {
  WIDENED_FIELD,
};

struct ClassBasesNarrowsFieldError {
  ClassBasesNarrowsFieldErrorCode code;
  std::string message;
};

}
