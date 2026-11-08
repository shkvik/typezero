#pragma once

#include "llvm/ADT/StringRef.h"
#include <string>

namespace tz {

enum class ClassLinksNotAFieldErrorCode {
  FIELD_OF_PARENT,
};

struct ClassLinksNotAFieldError {
  ClassLinksNotAFieldErrorCode code;
  std::string message;
};

class ErrorTextNotAField {
public:
  static std::string fieldOf(llvm::StringRef name, llvm::StringRef owner);
};

enum class ClassLinksOverrideTargetErrorCode {
  NO_PARENT,
  NO_SUCH_METHOD,
  NO_MATCHING_PARAMS,
};

struct ClassLinksOverrideTargetError {
  ClassLinksOverrideTargetErrorCode code;
  std::string message;
};

class ErrorTextOverrideTarget {
public:
  static std::string noParent(llvm::StringRef name, llvm::StringRef cls);
  static std::string noMethod(llvm::StringRef name, llvm::StringRef parent);
  static std::string noParams(llvm::StringRef name, llvm::StringRef parent, bool one,
                              llvm::StringRef list);
};

enum class ClassLinksArrowOverrideErrorCode {
  ARROW_IN_BASE,
};

struct ClassLinksArrowOverrideError {
  ClassLinksArrowOverrideErrorCode code;
  std::string message;
};

class ErrorTextArrowOverride {
public:
  static std::string arrow(llvm::StringRef name, llvm::StringRef base);
};

enum class ClassLinksBaseVisibleErrorCode {
  PRIVATE_IN_BASE,
};

struct ClassLinksBaseVisibleError {
  ClassLinksBaseVisibleErrorCode code;
  std::string message;
};

class ErrorTextBaseVisible {
public:
  static std::string hidden(llvm::StringRef name, llvm::StringRef base);
};

enum class ClassLinksAccessKeptErrorCode {
  NARROWER_ACCESS,
};

struct ClassLinksAccessKeptError {
  ClassLinksAccessKeptErrorCode code;
  std::string message;
};

class ErrorTextAccessKept {
public:
  static std::string narrower(llvm::StringRef name, llvm::StringRef baseAccess,
                              llvm::StringRef base, llvm::StringRef access);
};

enum class ClassLinksBodyKeptErrorCode {
  ABSTRACT_AGAIN,
};

struct ClassLinksBodyKeptError {
  ClassLinksBodyKeptErrorCode code;
  std::string message;
};

class ErrorTextBodyKept {
public:
  static std::string again(llvm::StringRef name, llvm::StringRef base, llvm::StringRef cls);
};

enum class ClassLinksSameTypeArgsErrorCode {
  OVERRIDE_WITH_TYPE_ARGUMENTS,
};

struct ClassLinksSameTypeArgsError {
  ClassLinksSameTypeArgsErrorCode code;
  std::string message;
};

class ErrorTextSameTypeArgs {
public:
  static std::string typeArgs(llvm::StringRef name, llvm::StringRef type, llvm::StringRef baseType,
                              llvm::StringRef baseName, llvm::StringRef baseClass);
};

class ErrorTextInheritedReturn {
public:
  static std::string returns(llvm::StringRef name, llvm::StringRef base, llvm::StringRef type);
};

}
