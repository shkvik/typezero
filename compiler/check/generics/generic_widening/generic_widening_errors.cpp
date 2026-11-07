#include "compiler/check/generics/generic_widening/generic_widening_errors.h"
#include "llvm/ADT/Twine.h"

namespace tz {

std::string ErrorTextVariance::takes(llvm::StringRef method, llvm::StringRef name) {
  return (llvm::Twine("its public method '") + method + "' takes " + name).str();
}

std::string ErrorTextVariance::settable(llvm::StringRef field) {
  return (llvm::Twine("its public field '") + field + "' can be set").str();
}

std::string ErrorTextVariance::baseTakes(llvm::StringRef name) {
  return (llvm::Twine("its base class takes ") + name + " in").str();
}

std::string ErrorTextVariance::sampleName() {
  return "MyError";
}

std::string ErrorTextVariance::noExtends(llvm::StringRef own, llvm::StringRef wide,
                                         llvm::StringRef example) {
  return (llvm::Twine("; '") + own + "' does not extend '" + wide +
          "': a type goes where a wider one is wanted only when it says so, e.g. "
          "'interface " +
          example + " extends " + wide + " { ... }'")
      .str();
}

std::string ErrorTextVariance::wider(llvm::StringRef generic, llvm::StringRef name,
                                     llvm::StringRef writer) {
  return (llvm::Twine("; '") + generic + "' with a wider " + name +
          " would let a value of another type into it: " + writer)
      .str();
}

}
