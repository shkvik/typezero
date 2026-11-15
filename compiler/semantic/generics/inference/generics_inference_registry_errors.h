#pragma once

#include "llvm/ADT/StringRef.h"
#include <string>

namespace tz {

enum class GenericsInferenceRegistryParamInferredErrorCode {
  CANNOT_INFER,
};

struct GenericsInferenceRegistryParamInferredError {
  GenericsInferenceRegistryParamInferredErrorCode code;
  std::string message;
};

enum class GenericsInferenceRegistryIterableFitsErrorCode {
  WRONG_ELEMENTS,
  NOT_ITERABLE,
};

struct GenericsInferenceRegistryIterableFitsError {
  GenericsInferenceRegistryIterableFitsErrorCode code;
  std::string message;
};

enum class GenericsInferenceRegistryConstraintMetErrorCode {
  CONSTRAINT_UNSATISFIED,
};

struct GenericsInferenceRegistryConstraintMetError {
  GenericsInferenceRegistryConstraintMetErrorCode code;
  std::string message;
};

enum class GenericsInferenceRegistryTypeArgCountErrorCode {
  TOO_MANY_TYPE_ARGUMENTS,
};

struct GenericsInferenceRegistryTypeArgCountError {
  GenericsInferenceRegistryTypeArgCountErrorCode code;
  std::string message;
};

enum class GenericsInferenceRegistryCandidatesJoinErrorCode {
  CONFLICTING_CANDIDATES,
};

struct GenericsInferenceRegistryCandidatesJoinError {
  GenericsInferenceRegistryCandidatesJoinErrorCode code;
  std::string message;
};

enum class GenericsInferenceRegistryJoinCandidatesErrorCode {
  CANDIDATES_CLASH,
};

struct GenericsInferenceRegistryJoinCandidatesError {
  GenericsInferenceRegistryJoinCandidatesErrorCode code;
  std::string message;
};

enum class GenericsInferenceRegistryCallbackParamErrorCode {
  CALLBACK_PARAM_UNINFERRED,
};

struct GenericsInferenceRegistryCallbackParamError {
  GenericsInferenceRegistryCallbackParamErrorCode code;
  std::string message;
};

enum class GenericsInferenceRegistryNotGrowingErrorCode {
  GROWING_TYPE_ARGUMENT,
};

struct GenericsInferenceRegistryNotGrowingError {
  GenericsInferenceRegistryNotGrowingErrorCode code;
  std::string message;
};

enum class GenericsInferenceRegistrySpreadFreeErrorCode {
  SPREAD_ARGUMENT,
};

struct GenericsInferenceRegistrySpreadFreeError {
  GenericsInferenceRegistrySpreadFreeErrorCode code;
  std::string message;
};

enum class GenericsInferenceRegistryArityErrorCode {
  WRONG_COUNT,
  WRONG_COUNT_TAGGED,
};

struct GenericsInferenceRegistryArityError {
  GenericsInferenceRegistryArityErrorCode code;
  std::string message;
};

enum class GenericsInferenceRegistryOwnClassReturnErrorCode {
  RETURNS_LITERAL_CLASS,
};

struct GenericsInferenceRegistryOwnClassReturnError {
  GenericsInferenceRegistryOwnClassReturnErrorCode code;
  std::string message;
};

class ErrorTextTypeArgSample {
public:
  static std::string single();
  static std::string list();
};

class ErrorTextParamInferred {
public:
  static std::string cannot(llvm::StringRef param, llvm::StringRef name, bool call,
                            llvm::StringRef ints);
};

class ErrorTextIterableFits {
public:
  static std::string elements(llvm::StringRef bound, llvm::StringRef given, llvm::StringRef wanted,
                              llvm::StringRef name);
  static std::string notIter(llvm::StringRef bound, llvm::StringRef wanted, llvm::StringRef name);
};

class ErrorTextConstraintMet {
public:
  static std::string unsatisfied(llvm::StringRef bound, llvm::StringRef constraint,
                                 llvm::StringRef param, llvm::StringRef name);
};

class ErrorTextTypeArgCount {
public:
  static std::string tooMany(llvm::StringRef name, size_t count, size_t got);
};

class ErrorTextCandidates {
public:
  static std::string conflict(llvm::StringRef param, llvm::StringRef name, llvm::StringRef type,
                              llvm::StringRef other);
};

class ErrorTextCallbackParam {
public:
  static std::string param(llvm::StringRef name);
  static std::string params();
  static std::string uninferred(llvm::StringRef param, llvm::StringRef name, llvm::StringRef what,
                                bool function);
};

class ErrorTextNotGrowing {
public:
  static std::string grows(llvm::StringRef name, llvm::StringRef bound, llvm::StringRef param);
};

}
