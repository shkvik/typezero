#pragma once

#include "llvm/ADT/StringRef.h"
#include <string>

namespace tz {

enum class GlobalFunctionCheckerCheckGlobalCallErrorCode {
  NOT_A_GLOBAL,
  ALREADY_REPORTED,
  CALL_FAILED,
  ARGUMENT_FAILED,
};

struct GlobalFunctionCheckerCheckGlobalCallError {
  GlobalFunctionCheckerCheckGlobalCallErrorCode code;
  std::string message;
};

enum class GlobalFunctionCheckerCheckGlobalFunctionErrorCode {
  ALREADY_REPORTED,
  UNESCAPE_OF_ENCODE,
  LEGACY_ESCAPE,
  LEGACY_UNESCAPE,
  DECODE_OF_ESCAPE,
  WRONG_ARITY,
  NUMBER_OF_UNDEFINED,
  NUMBER_OF_WRONG_TYPE,
};

struct GlobalFunctionCheckerCheckGlobalFunctionError {
  GlobalFunctionCheckerCheckGlobalFunctionErrorCode code;
  std::string message;
};

class ErrorTextGlobalFunction {
public:
  static std::string toBytes();
  static std::string escape();
  static std::string unescape();
  static std::string fromBytes();
  static std::string undefined();
  static std::string wrongType(llvm::StringRef type);
};

}
