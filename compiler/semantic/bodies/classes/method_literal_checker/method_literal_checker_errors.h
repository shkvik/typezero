#pragma once

#include "llvm/ADT/StringRef.h"
#include <string>

namespace tz {

enum class MethodLiteralCheckerNoSpreadErrorCode {
  SPREAD_IN_LITERAL,
};

struct MethodLiteralCheckerNoSpreadError {
  MethodLiteralCheckerNoSpreadErrorCode code;
  std::string message;
};

class ErrorTextLiteralSpread {
public:
  static std::string spread();
};

enum class MethodLiteralCheckerPlainMethodErrorCode {
  GENERIC_METHOD,
};

struct MethodLiteralCheckerPlainMethodError {
  MethodLiteralCheckerPlainMethodErrorCode code;
  std::string message;
};

class ErrorTextPlainMethod {
public:
  static std::string generic();
};

enum class MethodLiteralCheckerFieldValueErrorCode {
  VOID_VALUE,
  UNDEFINED_VALUE,
};

struct MethodLiteralCheckerFieldValueError {
  MethodLiteralCheckerFieldValueErrorCode code;
  std::string message;
};

class ErrorTextFieldValue {
public:
  static std::string label(llvm::StringRef name);
  static std::string isVoid(llvm::StringRef label);
  static std::string undefined(llvm::StringRef label);
};

enum class MethodLiteralCheckerPropertyFitsErrorCode {
  NO_SUCH_FIELD,
  NOT_A_METHOD,
  REPLACEABLE_FIELD,
  METHOD_TYPE_DIFFERS,
};

struct MethodLiteralCheckerPropertyFitsError {
  MethodLiteralCheckerPropertyFitsErrorCode code;
  std::string message;
};

class ErrorTextLiteralFits {
public:
  static std::string noField(llvm::StringRef target, llvm::StringRef name);
  static std::string notMethod(llvm::StringRef target, llvm::StringRef name, llvm::StringRef type);
  static std::string replaced(llvm::StringRef target, llvm::StringRef name);
  static std::string otherType(llvm::StringRef target, llvm::StringRef name, llvm::StringRef mine,
                               llvm::StringRef theirs);
};

enum class MethodLiteralCheckerMembersMatchErrorCode {
  MISSING_MEMBER,
  OVERLOADED_METHOD,
};

struct MethodLiteralCheckerMembersMatchError {
  MethodLiteralCheckerMembersMatchErrorCode code;
  std::string message;
};

class ErrorTextLiteralMembers {
public:
  static std::string missing(llvm::StringRef target, llvm::StringRef member);
  static std::string overloads(llvm::StringRef target, llvm::StringRef method);
};

}
