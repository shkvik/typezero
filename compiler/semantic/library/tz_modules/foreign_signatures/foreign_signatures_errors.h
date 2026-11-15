#pragma once

#include "llvm/ADT/StringRef.h"
#include <string>

namespace tz {

enum class ForeignSignaturesPlainSignatureErrorCode {
  TYPE_PARAMS_OR_REST,
};

struct ForeignSignaturesPlainSignatureError {
  ForeignSignaturesPlainSignatureErrorCode code;
  std::string message;
};

enum class ForeignSignaturesForeignParamErrorCode {
  OPTIONAL_PARAM,
  PARAM_TYPE,
};

struct ForeignSignaturesForeignParamError {
  ForeignSignaturesForeignParamErrorCode code;
  std::string message;
};

enum class ForeignSignaturesForeignReturnErrorCode {
  NO_RETURN_TYPE,
  RETURN_TYPE,
};

struct ForeignSignaturesForeignReturnError {
  ForeignSignaturesForeignReturnErrorCode code;
  std::string message;
};

class ErrorTextForeign {
public:
  static std::string plain();
  static std::string optional();
  static std::string takes(llvm::StringRef type);
  static std::string gives(llvm::StringRef type);
  static std::string noReturn();
};

}
