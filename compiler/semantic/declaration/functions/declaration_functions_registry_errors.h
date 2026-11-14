#pragma once

#include "llvm/ADT/StringRef.h"
#include <string>

namespace tz {

enum class DeclarationFunctionsRegistryTopLevelGenericErrorCode {
  NESTED_GENERIC,
};

struct DeclarationFunctionsRegistryTopLevelGenericError {
  DeclarationFunctionsRegistryTopLevelGenericErrorCode code;
  std::string message;
};

enum class DeclarationFunctionsRegistryParamDecoratorsErrorCode {
  DECORATOR_NOT_ALLOWED,
};

struct DeclarationFunctionsRegistryParamDecoratorsError {
  DeclarationFunctionsRegistryParamDecoratorsErrorCode code;
  std::string message;
};

enum class DeclarationFunctionsRegistryThisParamErrorCode {
  ARROW_THIS,
  METHOD_THIS,
};

struct DeclarationFunctionsRegistryThisParamError {
  DeclarationFunctionsRegistryThisParamErrorCode code;
  std::string message;
};

enum class DeclarationFunctionsRegistryRestTypeErrorCode {
  REST_NOT_ARRAY,
};

struct DeclarationFunctionsRegistryRestTypeError {
  DeclarationFunctionsRegistryRestTypeErrorCode code;
  std::string message;
};

enum class DeclarationFunctionsRegistryRequiredOrderErrorCode {
  REQUIRED_AFTER_OPTIONAL,
};

struct DeclarationFunctionsRegistryRequiredOrderError {
  DeclarationFunctionsRegistryRequiredOrderErrorCode code;
  std::string message;
};

enum class DeclarationFunctionsRegistryParamTypeKnownErrorCode {
  NEEDS_TYPE,
  DEFAULT_NOT_LITERAL,
};

struct DeclarationFunctionsRegistryParamTypeKnownError {
  DeclarationFunctionsRegistryParamTypeKnownErrorCode code;
  std::string message;
};

enum class DeclarationFunctionsRegistryNotAssertingErrorCode {
  ASSERTS_PREDICATE,
};

struct DeclarationFunctionsRegistryNotAssertingError {
  DeclarationFunctionsRegistryNotAssertingErrorCode code;
  std::string message;
};

enum class DeclarationFunctionsRegistryPredicateArgErrorCode {
  THIS_OUTSIDE_METHOD,
  NOT_A_PARAMETER,
};

struct DeclarationFunctionsRegistryPredicateArgError {
  DeclarationFunctionsRegistryPredicateArgErrorCode code;
  std::string message;
};

class ErrorTextFunctionDecl {
public:
  static std::string nested();
  static std::string decorator();
  static std::string needsType(llvm::StringRef label);
  static std::string defaultNeedsType(llvm::StringRef label);
  static std::string arrowThis();
  static std::string methodThis();
  static std::string restArray(llvm::StringRef name);
  static std::string required();
};

class ErrorTextAsserts {
public:
  static std::string notAllowed(llvm::StringRef shown, bool typed, llvm::StringRef call);
};

class ErrorTextPredicateArg {
public:
  static std::string thisOutside();
  static std::string notParam(llvm::StringRef name);
};

}
