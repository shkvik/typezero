#include "compiler/check/stmt/jump_checker/jump_checker_errors.h"
#include "llvm/ADT/Twine.h"

namespace tz {

std::string ErrorTextJump::noLabel(llvm::StringRef label) {
  return (llvm::Twine("cannot find label '") + label + "'").str();
}

std::string ErrorTextJump::breakOut() {
  return "'break' can only be used inside a loop or a 'switch'";
}

std::string ErrorTextJump::continueOut() {
  return "'continue' can only be used inside a loop";
}

std::string ErrorTextJump::notLoop(llvm::StringRef keyword, llvm::StringRef label) {
  return (llvm::Twine("'") + keyword + " " + label + "' needs a loop, and '" + label +
          "' does not label one")
      .str();
}

std::string ErrorTextLabel::duplicate(llvm::StringRef label) {
  return (llvm::Twine("duplicate label '") + label + "'").str();
}

std::string ErrorTextLabelBody::declared() {
  return "a declaration cannot have a label";
}

}
