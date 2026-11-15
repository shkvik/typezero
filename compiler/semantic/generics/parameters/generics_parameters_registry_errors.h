#pragma once

#include "llvm/ADT/StringRef.h"
#include <string>

namespace tz {

enum class GenericsParametersRegistryUniqueNameErrorCode {
  DUPLICATE_TYPE_PARAMETER,
};

struct GenericsParametersRegistryUniqueNameError {
  GenericsParametersRegistryUniqueNameErrorCode code;
  std::string message;
};

enum class GenericsParametersRegistryOwnNameErrorCode {
  BUILTIN_NAME,
};

struct GenericsParametersRegistryOwnNameError {
  GenericsParametersRegistryOwnNameErrorCode code;
  std::string message;
};

enum class GenericsParametersRegistryConstraintAloneErrorCode {
  UNDEFINED_CONSTRAINT,
};

struct GenericsParametersRegistryConstraintAloneError {
  GenericsParametersRegistryConstraintAloneErrorCode code;
  std::string message;
};

enum class GenericsParametersRegistryDefaultGivenErrorCode {
  MISSING_DEFAULT,
};

struct GenericsParametersRegistryDefaultGivenError {
  GenericsParametersRegistryDefaultGivenErrorCode code;
  std::string message;
};

enum class GenericsParametersRegistryDefaultFitsErrorCode {
  DEFAULT_UNSATISFIED,
};

struct GenericsParametersRegistryDefaultFitsError {
  GenericsParametersRegistryDefaultFitsErrorCode code;
  std::string message;
};

class ErrorTextUniqueParam {
public:
  static std::string duplicate(llvm::StringRef name);
};

class ErrorTextOwnName {
public:
  static std::string builtin(llvm::StringRef name);
};

class ErrorTextConstraint {
public:
  static std::string undefined(llvm::StringRef name);
};

class ErrorTextDefaultFits {
public:
  static std::string unsatisfied(llvm::StringRef fallback, llvm::StringRef name,
                                 llvm::StringRef constraint);
};

class ErrorTextDefaultGiven {
public:
  static std::string missing(llvm::StringRef name);
};

}
