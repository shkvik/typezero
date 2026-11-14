#pragma once

#include "llvm/ADT/StringRef.h"
#include <string>

namespace tz {

enum class DeclarationLinksRegistryNotAFieldErrorCode {
  FIELD_OF_PARENT,
};

struct DeclarationLinksRegistryNotAFieldError {
  DeclarationLinksRegistryNotAFieldErrorCode code;
  std::string message;
};

enum class DeclarationLinksRegistryOverrideTargetErrorCode {
  NO_PARENT,
  NO_SUCH_METHOD,
  NO_MATCHING_PARAMS,
};

struct DeclarationLinksRegistryOverrideTargetError {
  DeclarationLinksRegistryOverrideTargetErrorCode code;
  std::string message;
};

enum class DeclarationLinksRegistryArrowOverrideErrorCode {
  ARROW_IN_BASE,
};

struct DeclarationLinksRegistryArrowOverrideError {
  DeclarationLinksRegistryArrowOverrideErrorCode code;
  std::string message;
};

enum class DeclarationLinksRegistryBaseVisibleErrorCode {
  PRIVATE_IN_BASE,
};

struct DeclarationLinksRegistryBaseVisibleError {
  DeclarationLinksRegistryBaseVisibleErrorCode code;
  std::string message;
};

enum class DeclarationLinksRegistryAccessKeptErrorCode {
  NARROWER_ACCESS,
};

struct DeclarationLinksRegistryAccessKeptError {
  DeclarationLinksRegistryAccessKeptErrorCode code;
  std::string message;
};

enum class DeclarationLinksRegistryBodyKeptErrorCode {
  ABSTRACT_AGAIN,
};

struct DeclarationLinksRegistryBodyKeptError {
  DeclarationLinksRegistryBodyKeptErrorCode code;
  std::string message;
};

enum class DeclarationLinksRegistrySameTypeArgsErrorCode {
  OVERRIDE_WITH_TYPE_ARGUMENTS,
};

struct DeclarationLinksRegistrySameTypeArgsError {
  DeclarationLinksRegistrySameTypeArgsErrorCode code;
  std::string message;
};

class ErrorTextNotAField {
public:
  static std::string fieldOf(llvm::StringRef name, llvm::StringRef owner);
};

class ErrorTextOverrideTarget {
public:
  static std::string noParent(llvm::StringRef name, llvm::StringRef cls);
  static std::string noMethod(llvm::StringRef name, llvm::StringRef parent);
  static std::string noParams(llvm::StringRef name, llvm::StringRef parent, bool one,
                              llvm::StringRef list);
};

class ErrorTextArrowOverride {
public:
  static std::string arrow(llvm::StringRef name, llvm::StringRef base);
};

class ErrorTextBaseVisible {
public:
  static std::string hidden(llvm::StringRef name, llvm::StringRef base);
};

class ErrorTextAccessKept {
public:
  static std::string narrower(llvm::StringRef name, llvm::StringRef baseAccess,
                              llvm::StringRef base, llvm::StringRef access);
};

class ErrorTextBodyKept {
public:
  static std::string again(llvm::StringRef name, llvm::StringRef base, llvm::StringRef cls);
};

class ErrorTextSameTypeArgs {
public:
  static std::string typeArgs(llvm::StringRef name, llvm::StringRef type, llvm::StringRef baseType,
                              llvm::StringRef baseName, llvm::StringRef baseClass);
};

}
