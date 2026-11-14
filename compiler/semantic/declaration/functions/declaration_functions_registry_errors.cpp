#include "compiler/semantic/declaration/functions/declaration_functions_registry_errors.h"
#include "llvm/ADT/Twine.h"

namespace tz {

std::string ErrorTextFunctionDecl::nested() {
  return "a generic function can be declared only at the top level for now";
}

std::string ErrorTextFunctionDecl::decorator() {
  return "a decorator of a parameter can decorate only a "
         "parameter of a method or a constructor of a class";
}

std::string ErrorTextFunctionDecl::needsType(llvm::StringRef label) {
  return (llvm::Twine("parameter '") + label + "' needs a type annotation").str();
}

std::string ErrorTextFunctionDecl::defaultNeedsType(llvm::StringRef label) {
  return (llvm::Twine("parameter '") + label +
          "' needs a type: only a literal default value (a number, a string, true or false) "
          "gives one, e.g. '" +
          label + ": int = measure()'")
      .str();
}

std::string ErrorTextFunctionDecl::arrowThis() {
  return "an arrow function cannot have a 'this' parameter: it sees the "
         "'this' of the function around it; write a 'function'";
}

std::string ErrorTextFunctionDecl::methodThis() {
  return "a method cannot have a 'this' parameter: its 'this' is its object";
}

std::string ErrorTextFunctionDecl::restArray(llvm::StringRef name) {
  return (llvm::Twine("a rest parameter must be an array, e.g. '...") + name + ": int[]'").str();
}

std::string ErrorTextFunctionDecl::required() {
  return "a required parameter cannot follow an optional one";
}

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

}
