#pragma once

#include "llvm/ADT/StringRef.h"
#include <string>

namespace tz {

enum class GenericInstantiatorTypeArgCountErrorCode {
  TOO_MANY_TYPE_ARGUMENTS,
};

struct GenericInstantiatorTypeArgCountError {
  GenericInstantiatorTypeArgCountErrorCode code;
  std::string message;
};

class ErrorTextGenericCount {
public:
  static std::string tooMany(llvm::StringRef name, size_t count, size_t got);
};

enum class GenericInstantiatorTypeArgsGivenErrorCode {
  NEEDS_TYPE_ARGUMENTS,
};

struct GenericInstantiatorTypeArgsGivenError {
  GenericInstantiatorTypeArgsGivenErrorCode code;
  std::string message;
};

class ErrorTextGenericValue {
public:
  static std::string needsArgs(llvm::StringRef name);
};

enum class GenericInstantiatorOwnClassReturnErrorCode {
  RETURNS_LITERAL_CLASS,
};

struct GenericInstantiatorOwnClassReturnError {
  GenericInstantiatorOwnClassReturnErrorCode code;
  std::string message;
};

class ErrorTextOwnClass {
public:
  static std::string literal(llvm::StringRef name);
};

enum class GenericInstantiatorChainBoundedErrorCode {
  ENDLESS_CHAIN,
};

struct GenericInstantiatorChainBoundedError {
  GenericInstantiatorChainBoundedErrorCode code;
  std::string message;
};

class ErrorTextInstanceChain {
public:
  static std::string endless(llvm::StringRef name, llvm::StringRef types);
};

enum class GenericInstantiatorParamsMeaningfulErrorCode {
  PARAMS_MEANINGLESS,
};

struct GenericInstantiatorParamsMeaningfulError {
  GenericInstantiatorParamsMeaningfulErrorCode code;
  std::string message;
};

class ErrorTextInstanceParams {
public:
  static std::string meaningless(llvm::StringRef name, llvm::StringRef bindings,
                                 llvm::StringRef param);
};

class ErrorTextInstanceCall {
public:
  static std::string cannotCall(llvm::StringRef name, llvm::StringRef bindings,
                                llvm::StringRef message);
  static std::string inWhich(llvm::StringRef name, bool uses, llvm::StringRef next);
  static std::string here(llvm::StringRef name);
};

}
