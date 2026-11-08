#include "compiler/semantic/generics/generic_class_checker/generic_class_checker_errors.h"
#include "llvm/ADT/Twine.h"

namespace tz {

std::string ErrorTextInstanceUse::cannotUse(llvm::StringRef name, llvm::StringRef bindings,
                                            llvm::StringRef message) {
  return (llvm::Twine("cannot use '") + name + "' with " + bindings + ": " + message).str();
}

std::string ErrorTextInstanceUse::usesHere(llvm::StringRef name, llvm::StringRef next) {
  return (llvm::Twine("in '") + name + "', which uses '" + next + "' here").str();
}

std::string ErrorTextInstanceUse::here(llvm::StringRef name) {
  return (llvm::Twine("in '") + name + "', here").str();
}

std::string ErrorTextDeclaredFirst::before(llvm::StringRef name) {
  return (llvm::Twine("class '") + name +
          "' is used before its declaration, which JavaScript has not run "
          "yet here (a ReferenceError); move the class up")
      .str();
}

std::string ErrorTextConcrete::abstract(llvm::StringRef name) {
  return (llvm::Twine("class '") + name +
          "' is abstract and cannot be created: create "
          "an object of a class that extends it")
      .str();
}

std::string ErrorTextConstructorFit::inferArgs(llvm::StringRef name, bool none, size_t count) {
  return (llvm::Twine("cannot infer the type arguments of '") + name + "': " +
          (none ? "no constructor takes " : "several constructors take ") + std::to_string(count) +
          " argument" + (count == 1 ? "" : "s") + "; give them, e.g. 'new " + name + "<int>(...)'")
      .str();
}

}
