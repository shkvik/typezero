#include "compiler/semantic/declaration/names/declaration_names_registry/declaration_names_registry_errors.h"
#include "llvm/ADT/Twine.h"

namespace tz {

std::string ErrorTextDeclareName::undefined() {
  return "'undefined' cannot be used as a name";
}

std::string ErrorTextDeclareName::previous() {
  return "previously declared here";
}

std::string ErrorTextDeclareName::function(llvm::StringRef name) {
  return (llvm::Twine("duplicate declaration of '") + name +
          "'; plain functions are not overloaded: give each variant its own name")
      .str();
}

std::string ErrorTextDeclareName::duplicate(llvm::StringRef name) {
  return (llvm::Twine("duplicate declaration of '") + name + "'").str();
}

std::string ErrorTextCheckVisible::otherCase(llvm::StringRef name) {
  return (llvm::Twine("'") + name +
          "' is declared in an earlier case of the 'switch'; declare it "
          "before the 'switch' to use it in several cases")
      .str();
}

}
