#include "compiler/semantic/flow/predicate_checker/predicate_checker_errors.h"
#include "llvm/ADT/Twine.h"

namespace tz {

std::string ErrorTextAsserts::notAllowed(llvm::StringRef shown, bool typed, llvm::StringRef call) {
  std::string written = typed ? shown.str() : std::string("boolean");
  std::string message = (llvm::Twine("'asserts ") + shown +
                         "' is not supported (docs/narrowing.md): there are no exceptions, so a "
                         "function cannot stop its caller; write '" +
                         written + "' and return whether it holds")
                            .str();
  if (!call.empty()) {
    message +=
        (llvm::Twine(", then check it at the call: 'if (!") + call + ") return err(...)'").str();
  }

  return message;
}

std::string ErrorTextPredicateArg::thisOutside() {
  return "'this is T' can only be the return type of a method";
}

std::string ErrorTextPredicateArg::notParam(llvm::StringRef name) {
  return (llvm::Twine("a type predicate names a parameter of the function, and '") + name +
          "' is none")
      .str();
}

std::string ErrorTextPredicateExit::mayLie(llvm::StringRef function, llvm::StringRef var,
                                           llvm::StringRef type, llvm::StringRef shown) {
  return (llvm::Twine(function) + " says '" + var + " is " + type +
          "', but this may give true while '" + var + "' is only known to be '" + shown +
          "'; show it with what narrows types: a union's tag, 'typeof', "
          "'instanceof', '!= undefined', 'Array.isArray' or a function that tests "
          "it (checks of fields alone do not make an object of a type)")
      .str();
}

}
