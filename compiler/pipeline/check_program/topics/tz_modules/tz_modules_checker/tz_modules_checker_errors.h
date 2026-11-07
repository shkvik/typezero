#pragma once

#include "llvm/ADT/StringRef.h"
#include <string>

namespace tz {

enum class TzModulesCheckerPlainSignatureErrorCode {
  TYPE_PARAMS_OR_REST,
};

struct TzModulesCheckerPlainSignatureError {
  TzModulesCheckerPlainSignatureErrorCode code;
  std::string message;
};

enum class TzModulesCheckerExternalParamErrorCode {
  OPTIONAL_PARAM,
  PARAM_TYPE,
};

struct TzModulesCheckerExternalParamError {
  TzModulesCheckerExternalParamErrorCode code;
  std::string message;
};

enum class TzModulesCheckerExternalReturnErrorCode {
  RETURN_TYPE,
};

struct TzModulesCheckerExternalReturnError {
  TzModulesCheckerExternalReturnErrorCode code;
  std::string message;
};

class ErrorTextIntrinsic {
public:
  static std::string plain();
  static std::string optional();
  static std::string takes(llvm::StringRef type);
  static std::string gives(llvm::StringRef type);
};

class ErrorTextIntrinsicCall {
public:
  static std::string wrong(llvm::StringRef name);
};

}
