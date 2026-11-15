#pragma once

#include "llvm/ADT/StringRef.h"
#include <string>

namespace tz {

enum class GenericsFunctionsRegistryTypeArgCountErrorCode {
  TOO_MANY_TYPE_ARGUMENTS,
};

struct GenericsFunctionsRegistryTypeArgCountError {
  GenericsFunctionsRegistryTypeArgCountErrorCode code;
  std::string message;
};

enum class GenericsFunctionsRegistryTypeArgsGivenErrorCode {
  NEEDS_TYPE_ARGUMENTS,
};

struct GenericsFunctionsRegistryTypeArgsGivenError {
  GenericsFunctionsRegistryTypeArgsGivenErrorCode code;
  std::string message;
};

enum class GenericsFunctionsRegistryOwnClassReturnErrorCode {
  RETURNS_LITERAL_CLASS,
};

struct GenericsFunctionsRegistryOwnClassReturnError {
  GenericsFunctionsRegistryOwnClassReturnErrorCode code;
  std::string message;
};

enum class GenericsFunctionsRegistryChainBoundedErrorCode {
  ENDLESS_CHAIN,
};

struct GenericsFunctionsRegistryChainBoundedError {
  GenericsFunctionsRegistryChainBoundedErrorCode code;
  std::string message;
};

enum class GenericsFunctionsRegistryParamsMeaningfulErrorCode {
  PARAMS_MEANINGLESS,
};

struct GenericsFunctionsRegistryParamsMeaningfulError {
  GenericsFunctionsRegistryParamsMeaningfulErrorCode code;
  std::string message;
};

class ErrorTextGenericCount {
public:
  static std::string tooMany(llvm::StringRef name, size_t count, size_t got);
};

class ErrorTextGenericValue {
public:
  static std::string needsArgs(llvm::StringRef name);
};

class ErrorTextOwnClass {
public:
  static std::string literal(llvm::StringRef name);
};

class ErrorTextInstanceChain {
public:
  static std::string endless(llvm::StringRef name, llvm::StringRef types);
};

class ErrorTextInstanceParams {
public:
  static std::string meaningless(llvm::StringRef name, llvm::StringRef bindings,
                                 llvm::StringRef param);
};

}
