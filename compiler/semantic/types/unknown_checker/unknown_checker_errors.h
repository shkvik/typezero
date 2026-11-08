#pragma once

#include "llvm/ADT/StringRef.h"
#include <string>

namespace tz {

enum class UnknownCheckerUnknownUseErrorCode {
  OBJECT_NOT_NARROWED,
  VALUE_NOT_NARROWED,
};

struct UnknownCheckerUnknownUseError {
  UnknownCheckerUnknownUseErrorCode code;
  std::string message;
};

class ErrorTextUnknownUse {
public:
  static std::string property(llvm::StringRef name);
  static std::string method(llvm::StringRef name);
  static std::string object(llvm::StringRef value, llvm::StringRef what);
  static std::string unknown(llvm::StringRef value, llvm::StringRef what);
};

enum class UnknownCheckerCheckUnknownMemberErrorCode {
  CLASS_MEMBER,
  NO_CONSTRUCTOR,
  NOT_NARROWED,
};

struct UnknownCheckerCheckUnknownMemberError {
  UnknownCheckerCheckUnknownMemberErrorCode code;
  std::string message;
};

class ErrorTextUnknownMember {
public:
  static std::string noCtor();
};

enum class UnknownCheckerCheckUnknownMethodErrorCode {
  NOT_UNKNOWN,
  RUNTIME_CLASS,
  CLASS_VALUE,
  NOT_NARROWED,
};

struct UnknownCheckerCheckUnknownMethodError {
  UnknownCheckerCheckUnknownMethodErrorCode code;
  std::string message;
};

class ErrorTextUnknownMethod {
public:
  static std::string runtime(llvm::StringRef type);
  static std::string classValue(llvm::StringRef name);
};

class ErrorTextUnknownHint {
public:
  static std::string instanceOf(llvm::StringRef value, llvm::StringRef cls);
  static std::string typeOf(llvm::StringRef value, llvm::StringRef type);
  static std::string isArray(llvm::StringRef value);
  static std::string anyClass(llvm::StringRef check);
  static std::string anything(llvm::StringRef check);
};

}
