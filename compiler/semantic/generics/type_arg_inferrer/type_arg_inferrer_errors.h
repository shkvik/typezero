#pragma once

#include "llvm/ADT/StringRef.h"
#include <string>

namespace tz {

enum class TypeArgInferrerParamInferredErrorCode {
  CANNOT_INFER,
};

struct TypeArgInferrerParamInferredError {
  TypeArgInferrerParamInferredErrorCode code;
  std::string message;
};

class ErrorTextParamInferred {
public:
  static std::string cannot(llvm::StringRef param, llvm::StringRef name, bool call,
                            llvm::StringRef ints);
};

enum class TypeArgInferrerIterableFitsErrorCode {
  WRONG_ELEMENTS,
  NOT_ITERABLE,
};

struct TypeArgInferrerIterableFitsError {
  TypeArgInferrerIterableFitsErrorCode code;
  std::string message;
};

class ErrorTextIterableFits {
public:
  static std::string elements(llvm::StringRef bound, llvm::StringRef given, llvm::StringRef wanted,
                              llvm::StringRef name);
  static std::string notIter(llvm::StringRef bound, llvm::StringRef wanted, llvm::StringRef name);
};

enum class TypeArgInferrerConstraintMetErrorCode {
  CONSTRAINT_UNSATISFIED,
};

struct TypeArgInferrerConstraintMetError {
  TypeArgInferrerConstraintMetErrorCode code;
  std::string message;
};

class ErrorTextConstraintMet {
public:
  static std::string unsatisfied(llvm::StringRef bound, llvm::StringRef constraint,
                                 llvm::StringRef param, llvm::StringRef name);
};

enum class TypeArgInferrerTypeArgCountErrorCode {
  TOO_MANY_TYPE_ARGUMENTS,
};

struct TypeArgInferrerTypeArgCountError {
  TypeArgInferrerTypeArgCountErrorCode code;
  std::string message;
};

class ErrorTextTypeArgCount {
public:
  static std::string tooMany(llvm::StringRef name, size_t count, size_t got);
};

enum class TypeArgInferrerCandidatesJoinErrorCode {
  CONFLICTING_CANDIDATES,
};

struct TypeArgInferrerCandidatesJoinError {
  TypeArgInferrerCandidatesJoinErrorCode code;
  std::string message;
};

enum class TypeArgInferrerJoinCandidatesErrorCode {
  CANDIDATES_CLASH,
};

struct TypeArgInferrerJoinCandidatesError {
  TypeArgInferrerJoinCandidatesErrorCode code;
  std::string message;
};

class ErrorTextCandidates {
public:
  static std::string conflict(llvm::StringRef param, llvm::StringRef name, llvm::StringRef type,
                              llvm::StringRef other);
};

enum class TypeArgInferrerCallbackParamErrorCode {
  CALLBACK_PARAM_UNINFERRED,
};

struct TypeArgInferrerCallbackParamError {
  TypeArgInferrerCallbackParamErrorCode code;
  std::string message;
};

class ErrorTextCallbackParam {
public:
  static std::string param(llvm::StringRef name);
  static std::string params();
  static std::string uninferred(llvm::StringRef param, llvm::StringRef name, llvm::StringRef what,
                                bool function);
};

enum class TypeArgInferrerNotGrowingErrorCode {
  GROWING_TYPE_ARGUMENT,
};

struct TypeArgInferrerNotGrowingError {
  TypeArgInferrerNotGrowingErrorCode code;
  std::string message;
};

class ErrorTextNotGrowing {
public:
  static std::string grows(llvm::StringRef name, llvm::StringRef bound, llvm::StringRef param);
};

}
