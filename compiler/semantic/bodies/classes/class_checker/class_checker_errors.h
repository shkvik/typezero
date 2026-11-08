#pragma once

#include "llvm/ADT/StringRef.h"
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

class ErrorTextThis {
public:
  static std::string undeclared(llvm::StringRef function);
};

enum class ClassCheckerCheckClassStmtErrorCode {
  NOT_TOP_LEVEL,
};

struct ClassCheckerCheckClassStmtError {
  ClassCheckerCheckClassStmtErrorCode code;
  std::string message;
};

class ErrorTextClassStmt {
public:
  static std::string nested(llvm::StringRef className);
};

enum class ClassCheckerCheckFieldsSetErrorCode {
  FIELD_NOT_SET_ON_EVERY_PATH,
};

struct ClassCheckerCheckFieldsSetError {
  ClassCheckerCheckFieldsSetErrorCode code;
  std::string message;
};

class ErrorTextFieldsSet {
public:
  static std::string pathUnset(llvm::StringRef className, llvm::StringRef field);
};

enum class ClassCheckerIsInitThisErrorCode {
  NOT_IN_CONSTRUCTOR,
  NOT_THIS,
};

struct ClassCheckerIsInitThisError {
  ClassCheckerIsInitThisErrorCode code;
  std::string message;
};

class ErrorTextInitThis {
public:
  static std::string outside();
  static std::string notThis();
};

enum class ClassCheckerDeclareThisErrorCode {
  NOT_A_METHOD,
};

struct ClassCheckerDeclareThisError {
  ClassCheckerDeclareThisErrorCode code;
  std::string message;
};

class ErrorTextDeclareThis {
public:
  static std::string notMethod(llvm::StringRef function);
};

enum class ClassCheckerHasDecoratorsErrorCode {
  NO_DECORATORS,
};

struct ClassCheckerHasDecoratorsError {
  ClassCheckerHasDecoratorsErrorCode code;
  std::string message;
};

class ErrorTextDecorators {
public:
  static std::string none(llvm::StringRef className);
};

enum class ClassCheckerThisReadyErrorCode {
  BEFORE_SUPER,
  FIELD_NOT_SET,
};

struct ClassCheckerThisReadyError {
  ClassCheckerThisReadyErrorCode code;
  std::string message;
};

class ErrorTextThisReady {
public:
  static std::string noSuper();
  static std::string unset(llvm::StringRef field);
};

enum class ClassCheckerFirstUnsetErrorCode {
  NOT_IN_CONSTRUCTOR,
  ALL_FIELDS_SET,
};

struct ClassCheckerFirstUnsetError {
  ClassCheckerFirstUnsetErrorCode code;
  std::string message;
};

class ErrorTextFirstUnset {
public:
  static std::string outside();
  static std::string allSet(llvm::StringRef className);
};

enum class ClassCheckerFieldStartValueErrorCode {
  NO_START_VALUE,
  NEVER_SET,
};

struct ClassCheckerFieldStartValueError {
  ClassCheckerFieldStartValueErrorCode code;
  std::string message;
};

class ErrorTextStartValue {
public:
  static std::string noStart(llvm::StringRef field, llvm::StringRef type);
  static std::string neverSet(llvm::StringRef field, llvm::StringRef type);
};

enum class ClassCheckerOverrideReturnErrorCode {
  RETURNS_OTHER_TYPE,
};

struct ClassCheckerOverrideReturnError {
  ClassCheckerOverrideReturnErrorCode code;
  std::string message;
};

class ErrorTextOverride {
public:
  static std::string otherType(llvm::StringRef method, llvm::StringRef mine,
                               llvm::StringRef className, llvm::StringRef theirs,
                               llvm::StringRef baseClass);
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

class ErrorTextThisOf {
public:
  static std::string outside();
  static std::string inField(llvm::StringRef className, llvm::StringRef member);
  static std::string inStatic(llvm::StringRef className, llvm::StringRef member);
  static std::string inFunction();
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

class ErrorTextReadyThis {
public:
  static std::string inClosure();
  static std::string noSuper(llvm::StringRef parentClass);
  static std::string unset(llvm::StringRef field);
};

}
