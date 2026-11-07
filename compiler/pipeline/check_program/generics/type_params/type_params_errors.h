#pragma once

#include "llvm/ADT/StringRef.h"
#include <string>

namespace tz {

enum class TypeParamsUniqueNameErrorCode {
  DUPLICATE_TYPE_PARAMETER,
};

struct TypeParamsUniqueNameError {
  TypeParamsUniqueNameErrorCode code;
  std::string message;
};

class ErrorTextUniqueParam {
public:
  static std::string duplicate(llvm::StringRef name);
};

enum class TypeParamsOwnNameErrorCode {
  BUILTIN_NAME,
};

struct TypeParamsOwnNameError {
  TypeParamsOwnNameErrorCode code;
  std::string message;
};

class ErrorTextOwnName {
public:
  static std::string builtin(llvm::StringRef name);
};

enum class TypeParamsConstraintAloneErrorCode {
  UNDEFINED_CONSTRAINT,
};

struct TypeParamsConstraintAloneError {
  TypeParamsConstraintAloneErrorCode code;
  std::string message;
};

class ErrorTextConstraint {
public:
  static std::string undefined(llvm::StringRef name);
};

enum class TypeParamsDefaultFitsErrorCode {
  DEFAULT_UNSATISFIED,
};

struct TypeParamsDefaultFitsError {
  TypeParamsDefaultFitsErrorCode code;
  std::string message;
};

class ErrorTextDefaultFits {
public:
  static std::string unsatisfied(llvm::StringRef fallback, llvm::StringRef name,
                                 llvm::StringRef constraint);
};

enum class TypeParamsDefaultGivenErrorCode {
  MISSING_DEFAULT,
};

struct TypeParamsDefaultGivenError {
  TypeParamsDefaultGivenErrorCode code;
  std::string message;
};

class ErrorTextDefaultGiven {
public:
  static std::string missing(llvm::StringRef name);
};

}
