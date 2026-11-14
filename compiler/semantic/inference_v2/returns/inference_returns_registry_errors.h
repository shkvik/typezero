#pragma once

#include "llvm/ADT/StringRef.h"
#include <string>

namespace tz {

enum class InferenceReturnsRegistryEarlyCallErrorCode {
  CALLED_WHILE_LOADING,
};

struct InferenceReturnsRegistryEarlyCallError {
  InferenceReturnsRegistryEarlyCallErrorCode code;
  std::string message;
};

enum class InferenceReturnsRegistryCycleTypeErrorCode {
  DIFFERENT_TYPES,
  ONLY_SELF_CALLS,
  ONLY_CALLED_BACK,
};

struct InferenceReturnsRegistryCycleTypeError {
  InferenceReturnsRegistryCycleTypeErrorCode code;
  std::string message;
};

enum class InferenceReturnsRegistryCommonReturnErrorCode {
  DIFFERENT_TYPES,
};

struct InferenceReturnsRegistryCommonReturnError {
  InferenceReturnsRegistryCommonReturnErrorCode code;
  std::string message;
};

enum class InferenceReturnsRegistryDefinedReturnErrorCode {
  ONLY_UNDEFINED,
};

struct InferenceReturnsRegistryDefinedReturnError {
  InferenceReturnsRegistryDefinedReturnErrorCode code;
  std::string message;
};

enum class InferenceReturnsRegistryYieldsJoinErrorCode {
  TYPES_DIFFER,
};

struct InferenceReturnsRegistryYieldsJoinError {
  InferenceReturnsRegistryYieldsJoinErrorCode code;
  std::string message;
};

enum class InferenceReturnsRegistryYieldsKnownErrorCode {
  NOTHING_YIELDED,
};

struct InferenceReturnsRegistryYieldsKnownError {
  InferenceReturnsRegistryYieldsKnownErrorCode code;
  std::string message;
};

enum class InferenceReturnsRegistryGeneratorReturnsErrorCode {
  TYPES_DIFFER,
};

struct InferenceReturnsRegistryGeneratorReturnsError {
  InferenceReturnsRegistryGeneratorReturnsErrorCode code;
  std::string message;
};

class ErrorTextEarlyCall {
public:
  static std::string importBack(llvm::StringRef function);
};

class ErrorTextCycleType {
public:
  static std::string differ(llvm::StringRef function);
  static std::string selfOnly(llvm::StringRef function);
  static std::string calledBack(llvm::StringRef function);
};

class ErrorTextCommonReturn {
public:
  static std::string differ(llvm::StringRef function, llvm::StringRef here,
                            llvm::StringRef elsewhere);
};

class ErrorTextDefinedReturn {
public:
  static std::string undefined(llvm::StringRef function);
};

class ErrorTextYields {
public:
  static std::string differ(llvm::StringRef fn, llvm::StringRef here, llvm::StringRef elsewhere);
  static std::string unknown(llvm::StringRef fn, llvm::StringRef why);
  static std::string noYield();
  static std::string onlyUndef();
};

class ErrorTextGeneratorReturns {
public:
  static std::string differ(llvm::StringRef fn, llvm::StringRef here, llvm::StringRef elsewhere);
};

class ErrorTextInheritedReturn {
public:
  static std::string returns(llvm::StringRef name, llvm::StringRef base, llvm::StringRef type);
};

}
