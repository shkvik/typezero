#include "compiler/semantic/generics/type_params/type_params_errors.h"
#include "llvm/ADT/Twine.h"

namespace tz {

std::string ErrorTextUniqueParam::duplicate(llvm::StringRef name) {
  return (llvm::Twine("duplicate type parameter '") + name + "'").str();
}

std::string ErrorTextOwnName::builtin(llvm::StringRef name) {
  return (llvm::Twine("'") + name +
          "' is a built-in type and cannot name a type "
          "parameter")
      .str();
}

std::string ErrorTextConstraint::undefined(llvm::StringRef name) {
  return (llvm::Twine("the constraint of '") + name + "' cannot be 'undefined' alone").str();
}

std::string ErrorTextDefaultFits::unsatisfied(llvm::StringRef fallback, llvm::StringRef name,
                                              llvm::StringRef constraint) {
  return (llvm::Twine("the default '") + fallback + "' of '" + name +
          "' does not satisfy its constraint '" + constraint + "'")
      .str();
}

std::string ErrorTextDefaultGiven::missing(llvm::StringRef name) {
  return (llvm::Twine("type parameter '") + name + "' needs a default: it follows one that has one")
      .str();
}

}
