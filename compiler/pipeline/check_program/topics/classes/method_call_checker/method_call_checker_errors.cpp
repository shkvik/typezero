#include "compiler/pipeline/check_program/topics/classes/method_call_checker/method_call_checker_errors.h"
#include "llvm/ADT/Twine.h"

namespace tz {

std::string ErrorTextFieldCall::undefined() {
  return "the function may be undefined; check it first, or call it with "
         "'?.()', which gives undefined then";
}

std::string ErrorTextFieldCall::notFunc(llvm::StringRef name, llvm::StringRef type) {
  return (llvm::Twine("field '") + name + "' is not a function; it has type '" + type + "'").str();
}

std::string ErrorTextMethodLookup::isStatic(llvm::StringRef name, llvm::StringRef cls) {
  return (llvm::Twine("'") + name + "' is a static method of '" + cls +
          "': call it through the class, '" + cls + "." + name + "(...)'")
      .str();
}

std::string ErrorTextMethodLookup::noProperty(llvm::StringRef name, llvm::StringRef type) {
  return (llvm::Twine("property '") + name + "' does not exist on type '" + type + "'").str();
}

std::string ErrorTextUnsetCall::beforeSet(llvm::StringRef name, llvm::StringRef field) {
  return (llvm::Twine("'this.") + name + "()' is called before every field is set: field '" +
          field + "' is not set yet, and the method may read it")
      .str();
}

std::string ErrorTextMethodTypeArgs::notGeneric(llvm::StringRef name) {
  return (llvm::Twine("'") + name + "' is not a generic method; remove the type arguments").str();
}

std::string ErrorTextRuntimeNew::onlyFs(llvm::StringRef name) {
  return (llvm::Twine("'new ") + name + "' is not possible: only the functions of node:fs make " +
          name)
      .str();
}

std::string ErrorTextClassTypeArgs::notGeneric(llvm::StringRef name) {
  return (llvm::Twine("class '") + name + "' is not generic; remove the type arguments").str();
}

std::string ErrorTextClassOrder::usedEarly(llvm::StringRef name) {
  return (llvm::Twine("class '") + name +
          "' is used before its declaration, which JavaScript has not run "
          "yet here (a ReferenceError); move the class up")
      .str();
}

std::string ErrorTextAbstractNew::abstract(llvm::StringRef name) {
  return (llvm::Twine("class '") + name +
          "' is abstract and cannot be created: create "
          "an object of a class that extends it")
      .str();
}

std::string ErrorTextNoConstructor::noArgs(llvm::StringRef name) {
  return (llvm::Twine("class '") + name + "' has no constructor, so 'new " + name +
          "()' takes no arguments")
      .str();
}

}
