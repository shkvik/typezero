#pragma once

#include "llvm/ADT/StringRef.h"
#include <string>

namespace tz {

enum class ClassValueCheckerResolveConstructorTypeErrorCode {
  WRONG_TYPE_ARGUMENTS,
  NOT_AN_OBJECT_TYPE,
};

struct ClassValueCheckerResolveConstructorTypeError {
  ClassValueCheckerResolveConstructorTypeErrorCode code;
  std::string message;
};

class ErrorTextConstructorType {
public:
  static std::string typeArgs();
  static std::string notObject(llvm::StringRef type);
};

enum class ClassValueCheckerConstructorArgsErrorCode {
  NOT_IN_SUBCLASS,
  SEVERAL_CONSTRUCTORS,
  DECLARED_LATER,
};

struct ClassValueCheckerConstructorArgsError {
  ClassValueCheckerConstructorArgsErrorCode code;
  std::string message;
};

class ErrorTextConstructorArgs {
public:
  static std::string subclass();
  static std::string several(llvm::StringRef parent);
  static std::string later(llvm::StringRef parent);
};

enum class ClassValueCheckerCheckClassRefMemberErrorCode {
  NO_PROTOTYPES,
  RUNTIME_CLASS,
  VALUE_ONLY_CLASS,
};

struct ClassValueCheckerCheckClassRefMemberError {
  ClassValueCheckerCheckClassRefMemberErrorCode code;
  std::string message;
};

class ErrorTextClassRefMember {
public:
  static std::string prototype();
  static std::string runtime(llvm::StringRef type);
  static std::string nameOnly(llvm::StringRef name);
};

enum class ClassValueCheckerValueNewClassErrorCode {
  NOT_A_CLASS,
};

struct ClassValueCheckerValueNewClassError {
  ClassValueCheckerValueNewClassErrorCode code;
  std::string message;
};

class ErrorTextValueNew {
public:
  static std::string notClass(llvm::StringRef type);
  static std::string valueOnly(llvm::StringRef name);
  static std::string typeArgs();
  static std::string voidArg();
};

enum class ClassValueCheckerTypedClassErrorCode {
  VALUE_ONLY_CLASS,
};

struct ClassValueCheckerTypedClassError {
  ClassValueCheckerTypedClassErrorCode code;
  std::string message;
};

enum class ClassValueCheckerRuntimeTypeArgsErrorCode {
  TYPE_ARGUMENTS_GIVEN,
};

struct ClassValueCheckerRuntimeTypeArgsError {
  ClassValueCheckerRuntimeTypeArgsErrorCode code;
  std::string message;
};

enum class ClassValueCheckerArgumentValueErrorCode {
  VOID_ARGUMENT,
};

struct ClassValueCheckerArgumentValueError {
  ClassValueCheckerArgumentValueErrorCode code;
  std::string message;
};

enum class ClassValueCheckerClassDeclaredErrorCode {
  USED_BEFORE_DECLARATION,
};

struct ClassValueCheckerClassDeclaredError {
  ClassValueCheckerClassDeclaredErrorCode code;
  std::string message;
};

}
