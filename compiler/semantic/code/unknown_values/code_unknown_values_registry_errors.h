#pragma once

#include "llvm/ADT/StringRef.h"
#include <string>

namespace tz {

enum class CodeUnknownValuesRegistryUnknownUseErrorCode {
  OBJECT_NOT_NARROWED,
  VALUE_NOT_NARROWED,
};

struct CodeUnknownValuesRegistryUnknownUseError {
  CodeUnknownValuesRegistryUnknownUseErrorCode code;
  std::string message;
};

enum class CodeUnknownValuesRegistryCheckUnknownMemberErrorCode {
  CLASS_MEMBER,
  NO_CONSTRUCTOR,
  NOT_NARROWED,
};

struct CodeUnknownValuesRegistryCheckUnknownMemberError {
  CodeUnknownValuesRegistryCheckUnknownMemberErrorCode code;
  std::string message;
};

enum class CodeUnknownValuesRegistryCheckUnknownMethodErrorCode {
  NOT_UNKNOWN,
  RUNTIME_CLASS,
  CLASS_VALUE,
  NOT_NARROWED,
};

struct CodeUnknownValuesRegistryCheckUnknownMethodError {
  CodeUnknownValuesRegistryCheckUnknownMethodErrorCode code;
  std::string message;
};

class ErrorTextUnknownUse {
public:
  static std::string property(llvm::StringRef name);
  static std::string method(llvm::StringRef name);
  static std::string object(llvm::StringRef value, llvm::StringRef what);
  static std::string unknown(llvm::StringRef value, llvm::StringRef what);
};

class ErrorTextUnknownMember {
public:
  static std::string noCtor();
};

class ErrorTextUnknownMethod {
public:
  static std::string runtime(llvm::StringRef type);
  static std::string classValue(llvm::StringRef name);
};

}
