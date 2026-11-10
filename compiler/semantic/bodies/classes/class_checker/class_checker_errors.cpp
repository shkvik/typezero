#include "compiler/semantic/bodies/classes/class_checker/class_checker_errors.h"
#include "llvm/ADT/Twine.h"

namespace tz {

std::string ErrorTextThis::undeclared(llvm::StringRef function) {
  return (llvm::Twine("internal error: 'this' of '") + function +
          "' is used before the method is checked")
      .str();
}

std::string ErrorTextClassStmt::nested(llvm::StringRef className) {
  return (llvm::Twine("a class can be declared only at the top level of a file; here, "
                      "make it with a class expression, 'const ") +
          className + " = class { ... }'")
      .str();
}

std::string ErrorTextFieldsSet::pathUnset(llvm::StringRef className, llvm::StringRef field) {
  return (llvm::Twine("the constructor of '") + className + "' does not set field '" + field +
          "' on every path; set it on every path, or make it optional: '" + field + "?: T'")
      .str();
}

std::string ErrorTextInitThis::outside() {
  return "the code is not in the body of the constructor being checked";
}

std::string ErrorTextInitThis::notThis() {
  return "the expression is not the 'this' of the constructor";
}

std::string ErrorTextDeclareThis::notMethod(llvm::StringRef function) {
  return (llvm::Twine("internal error: 'this' is declared for '") + function +
          "', which is not a method")
      .str();
}

std::string ErrorTextDecorators::none(llvm::StringRef className) {
  return (llvm::Twine("class '") + className + "' has no decorators").str();
}

std::string ErrorTextThisReady::noSuper() {
  return "'super(...)' is not called yet";
}

std::string ErrorTextThisReady::unset(llvm::StringRef field) {
  return (llvm::Twine("field '") + field + "' is not set yet").str();
}

std::string ErrorTextFirstUnset::outside() {
  return "the code is not in a constructor";
}

std::string ErrorTextFirstUnset::allSet(llvm::StringRef className) {
  return (llvm::Twine("every field of '") + className + "' is set").str();
}

std::string ErrorTextStartValue::noStart(llvm::StringRef field, llvm::StringRef type) {
  return (llvm::Twine("field '") + field + "' has no value to start with: '" + type +
          "' has no empty value, like \"\" or 0; set it in a constructor, or make it optional: '" +
          field + "?: " + type + "'")
      .str();
}

std::string ErrorTextStartValue::neverSet(llvm::StringRef field, llvm::StringRef type) {
  return (llvm::Twine("field '") + field +
          "' never gets a value: set it in a constructor, or make it optional: '" + field + "?: " +
          type + "'")
      .str();
}

std::string ErrorTextOverride::otherType(llvm::StringRef method, llvm::StringRef mine,
                                         llvm::StringRef className, llvm::StringRef theirs,
                                         llvm::StringRef baseClass) {
  return (llvm::Twine("'") + method + "' returns '" + mine + "' in '" + className + "', and '" +
          theirs + "' in '" + baseClass +
          "', which it overrides; an override returns what the method it "
          "overrides returns")
      .str();
}

std::string ErrorTextThisOf::outside() {
  return "'this' is used outside of a method; it exists in the methods "
         "of classes and object literals";
}

std::string ErrorTextThisOf::inField(llvm::StringRef className, llvm::StringRef member) {
  return (llvm::Twine("in a static field 'this' would be the class itself; use the "
                      "class's name: '") +
          className + "." + member + "'")
      .str();
}

std::string ErrorTextThisOf::inStatic(llvm::StringRef className, llvm::StringRef member) {
  return (llvm::Twine("in a static method 'this' would be the class itself; use the "
                      "class's name: '") +
          className + "." + member + "'")
      .str();
}

std::string ErrorTextThisOf::inFunction() {
  return "a 'function' has no 'this' of its own; use an arrow function, "
         "which sees the 'this' of the method around it";
}

std::string ErrorTextReadyThis::inClosure() {
  return "this function uses 'this', and it is created before every field "
         "is set; create it after the constructor sets them";
}

std::string ErrorTextReadyThis::noSuper(llvm::StringRef parentClass) {
  return (llvm::Twine("'this' cannot be used before 'super(...)': the object exists once "
                      "the constructor of '") +
          parentClass + "' has run")
      .str();
}

std::string ErrorTextReadyThis::unset(llvm::StringRef field) {
  return (llvm::Twine("'this' is used before every field is set: field '") + field +
          "' is not set yet; until then 'this' can only set fields "
          "and read the ones set")
      .str();
}

}
