#include "compiler/pipeline/check_program/topics/tz_modules/tz_modules_checker/tz_modules_checker_errors.h"

namespace tz {

std::string ErrorTextIntrinsic::plain() {
  return "an intrinsic takes plain parameters: no type parameters, no rest";
}

std::string ErrorTextIntrinsic::optional() {
  return "an intrinsic has no optional parameters";
}

std::string ErrorTextIntrinsic::takes(llvm::StringRef type) {
  return "an intrinsic cannot take a value of type '" + type.str() + "'";
}

std::string ErrorTextIntrinsic::gives(llvm::StringRef type) {
  return "an intrinsic cannot give a value of type '" + type.str() + "'";
}

std::string ErrorTextIntrinsicCall::wrong(llvm::StringRef name) {
  return "'" + name.str() + "' is no intrinsic of tz, or is called wrongly";
}

}
