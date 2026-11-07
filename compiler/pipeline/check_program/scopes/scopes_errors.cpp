#include "compiler/pipeline/check_program/scopes/scopes_errors.h"
#include "llvm/ADT/Twine.h"

namespace tz {

std::string ErrorTextDeclare::undefined() {
  return "'undefined' cannot be used as a name";
}

std::string ErrorTextDeclare::duplicate(llvm::StringRef name) {
  return (llvm::Twine("duplicate declaration of '") + name + "'").str();
}

std::string ErrorTextDeclare::function(llvm::StringRef name) {
  return (llvm::Twine("duplicate declaration of '") + name +
          "'; plain functions are not overloaded: give each variant its own name")
      .str();
}

std::string ErrorTextDeclare::previous() {
  return "previously declared here";
}

std::string ErrorTextVisible::otherCase(llvm::StringRef name) {
  return (llvm::Twine("'") + name +
          "' is declared in an earlier case of the 'switch'; declare it "
          "before the 'switch' to use it in several cases")
      .str();
}

}
