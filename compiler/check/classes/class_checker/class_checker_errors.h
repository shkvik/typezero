#pragma once

#include "llvm/Support/SMLoc.h"
#include <string>

namespace tz {

enum class ClassCheckerCheckThisErrorCode {
  THIS_NOT_FOUND,
  THIS_NOT_DECLARED,
  THIS_NOT_READY,
};

struct ClassCheckerCheckThisError {
  ClassCheckerCheckThisErrorCode code;
  std::string message;
};

enum class ClassCheckerCheckClassStmtErrorCode {
  NOT_TOP_LEVEL,
};

struct ClassCheckerCheckClassStmtError {
  ClassCheckerCheckClassStmtErrorCode code;
  std::string message;
};

enum class ClassCheckerCheckFieldsSetErrorCode {
  FIELD_NOT_SET_ON_EVERY_PATH,
};

struct ClassCheckerCheckFieldsSetError {
  ClassCheckerCheckFieldsSetErrorCode code;
  std::string message;
};

enum class ClassCheckerCheckClassBodiesErrorCode {
  FIELD_WITHOUT_VALUE,
  OVERRIDE_RETURNS_OTHER_TYPE,
};

struct ClassCheckerCheckClassBodiesError {
  ClassCheckerCheckClassBodiesErrorCode code;
  std::string message;
  llvm::SMLoc loc;
};

enum class ClassCheckerIsInitThisErrorCode {
  NOT_IN_CONSTRUCTOR,
  NOT_THIS,
};

struct ClassCheckerIsInitThisError {
  ClassCheckerIsInitThisErrorCode code;
  std::string message;
};

enum class ClassCheckerDeclareThisErrorCode {
  NOT_A_METHOD,
};

struct ClassCheckerDeclareThisError {
  ClassCheckerDeclareThisErrorCode code;
  std::string message;
};

enum class ClassCheckerHasDecoratorsErrorCode {
  NO_DECORATORS,
};

struct ClassCheckerHasDecoratorsError {
  ClassCheckerHasDecoratorsErrorCode code;
  std::string message;
};

enum class ClassCheckerThisReadyErrorCode {
  BEFORE_SUPER,
  FIELD_NOT_SET,
};

struct ClassCheckerThisReadyError {
  ClassCheckerThisReadyErrorCode code;
  std::string message;
};

enum class ClassCheckerFirstUnsetErrorCode {
  NOT_IN_CONSTRUCTOR,
  ALL_FIELDS_SET,
};

struct ClassCheckerFirstUnsetError {
  ClassCheckerFirstUnsetErrorCode code;
  std::string message;
};

enum class ClassCheckerFieldStartValueErrorCode {
  NO_START_VALUE,
  NEVER_SET,
};

struct ClassCheckerFieldStartValueError {
  ClassCheckerFieldStartValueErrorCode code;
  std::string message;
};

enum class ClassCheckerOverrideReturnErrorCode {
  RETURNS_OTHER_TYPE,
};

struct ClassCheckerOverrideReturnError {
  ClassCheckerOverrideReturnErrorCode code;
  std::string message;
};

enum class ClassCheckerThisOfErrorCode {
  OUTSIDE_METHOD,
  IN_STATIC_FIELD,
  IN_STATIC_METHOD,
  IN_PLAIN_FUNCTION,
};

struct ClassCheckerThisOfError {
  ClassCheckerThisOfErrorCode code;
  std::string message;
};

enum class ClassCheckerReadyThisErrorCode {
  IN_CLOSURE_BEFORE_FIELDS_SET,
  BEFORE_SUPER,
  BEFORE_FIELDS_SET,
};

struct ClassCheckerReadyThisError {
  ClassCheckerReadyThisErrorCode code;
  std::string message;
};

}
