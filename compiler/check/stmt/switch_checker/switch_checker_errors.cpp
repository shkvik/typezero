#include "compiler/check/stmt/switch_checker/switch_checker_errors.h"
#include "llvm/ADT/Twine.h"

namespace tz {

std::string ErrorTextValueType::isVoid() {
  return "an expression of type 'void' cannot be compared";
}

std::string ErrorTextCaseType::mismatch(llvm::StringRef test, llvm::StringRef value) {
  return (llvm::Twine("a case of type '") + test +
          "' cannot be compared with a 'switch' value of type '" + value + "'")
      .str();
}

std::string ErrorTextCaseType::results(llvm::StringRef test, llvm::StringRef value) {
  return (llvm::Twine("a case of type '") + test +
          "' cannot be compared with a 'switch' value of type '" + value +
          "'; two Results are never equal, as they are two "
          "objects in JavaScript: switch on what they hold")
      .str();
}

std::string ErrorTextCasesLeft::unhandled(llvm::StringRef names, llvm::StringRef first) {
  return (llvm::Twine("the switch does not handle ") + names + "; add 'case " + first +
          ":', or 'default: break' if leaving it out is meant")
      .str();
}

}
