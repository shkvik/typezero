#pragma once

#include "llvm/ADT/StringRef.h"
#include <string>

namespace tz {

enum class MethodCallCheckerFieldCallableErrorCode {
  MAY_BE_UNDEFINED,
  NOT_A_FUNCTION,
  ALREADY_REPORTED,
};

struct MethodCallCheckerFieldCallableError {
  MethodCallCheckerFieldCallableErrorCode code;
  std::string message;
};

class ErrorTextFieldCall {
public:
  static std::string undefined();
  static std::string notFunc(llvm::StringRef name, llvm::StringRef type);
};

enum class MethodCallCheckerInstanceMethodsErrorCode {
  STATIC_METHOD,
  NO_SUCH_PROPERTY,
};

struct MethodCallCheckerInstanceMethodsError {
  MethodCallCheckerInstanceMethodsErrorCode code;
  std::string message;
};

class ErrorTextMethodLookup {
public:
  static std::string isStatic(llvm::StringRef name, llvm::StringRef cls);
  static std::string noProperty(llvm::StringRef name, llvm::StringRef type);
};

enum class MethodCallCheckerFieldsSetErrorCode {
  CALLED_BEFORE_SET,
};

struct MethodCallCheckerFieldsSetError {
  MethodCallCheckerFieldsSetErrorCode code;
  std::string message;
};

class ErrorTextUnsetCall {
public:
  static std::string beforeSet(llvm::StringRef name, llvm::StringRef field);
};

enum class MethodCallCheckerMethodTypeArgsErrorCode {
  NOT_GENERIC,
};

struct MethodCallCheckerMethodTypeArgsError {
  MethodCallCheckerMethodTypeArgsErrorCode code;
  std::string message;
};

class ErrorTextMethodTypeArgs {
public:
  static std::string notGeneric(llvm::StringRef name);
};

enum class MethodCallCheckerNotRuntimeErrorCode {
  RUNTIME_CLASS,
};

struct MethodCallCheckerNotRuntimeError {
  MethodCallCheckerNotRuntimeErrorCode code;
  std::string message;
};

class ErrorTextRuntimeNew {
public:
  static std::string onlyFs(llvm::StringRef name);
};

enum class MethodCallCheckerClassTypeArgsErrorCode {
  NOT_GENERIC,
};

struct MethodCallCheckerClassTypeArgsError {
  MethodCallCheckerClassTypeArgsErrorCode code;
  std::string message;
};

class ErrorTextClassTypeArgs {
public:
  static std::string notGeneric(llvm::StringRef name);
};

enum class MethodCallCheckerClassDeclaredErrorCode {
  USED_BEFORE_DECLARATION,
};

struct MethodCallCheckerClassDeclaredError {
  MethodCallCheckerClassDeclaredErrorCode code;
  std::string message;
};

class ErrorTextClassOrder {
public:
  static std::string usedEarly(llvm::StringRef name);
};

enum class MethodCallCheckerClassConcreteErrorCode {
  ABSTRACT_CLASS,
};

struct MethodCallCheckerClassConcreteError {
  MethodCallCheckerClassConcreteErrorCode code;
  std::string message;
};

class ErrorTextAbstractNew {
public:
  static std::string abstract(llvm::StringRef name);
};

enum class MethodCallCheckerArgumentsAbsentErrorCode {
  NO_CONSTRUCTOR,
};

struct MethodCallCheckerArgumentsAbsentError {
  MethodCallCheckerArgumentsAbsentErrorCode code;
  std::string message;
};

class ErrorTextNoConstructor {
public:
  static std::string noArgs(llvm::StringRef name);
};

}
