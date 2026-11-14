#include "compiler/semantic/semantic_state/v2/walk/scope_stack_context/scope_stack_context_errors.h"
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

}
