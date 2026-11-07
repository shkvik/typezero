#include "compiler/pipeline/check_program/topics/functions/function_checker/function_checker_errors.h"
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

std::string ErrorTextFunctionBody::noSuper(llvm::StringRef cls, llvm::StringRef parent) {
  return (llvm::Twine("the constructor of '") + cls + "' must call 'super(...)': '" + cls +
          "' extends '" + parent + "', whose constructor sets the fields it declares")
      .str();
}

std::string ErrorTextFunctionBody::forward(llvm::StringRef label, llvm::StringRef used) {
  return (llvm::Twine("the default value of '") + label + "' uses '" + used +
          "', which is not set yet: a default value can "
          "use only the parameters before it")
      .str();
}

std::string ErrorTextFunctionBody::noReturn(llvm::StringRef label, llvm::StringRef type) {
  return (llvm::Twine(label) + " must return a value of type '" + type + "' on every path").str();
}

std::string ErrorTextFunctionName::quoted(llvm::StringRef name) {
  return name.empty() ? "the function" : "'" + name.str() + "'";
}

std::string ErrorTextFunctionName::label(llvm::StringRef name) {
  return name.empty() ? "the function" : "function '" + name.str() + "'";
}

}
