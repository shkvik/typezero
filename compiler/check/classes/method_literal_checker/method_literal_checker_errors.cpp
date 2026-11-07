#include "compiler/check/classes/method_literal_checker/method_literal_checker_errors.h"
#include "llvm/ADT/Twine.h"

namespace tz {

std::string ErrorTextLiteralSpread::spread() {
  return "an object literal with methods cannot take the fields of another "
         "object with '...'; write them out";
}

std::string ErrorTextPlainMethod::generic() {
  return "a method of an object literal cannot be generic; declare a class, "
         "whose methods can";
}

std::string ErrorTextFieldValue::label(llvm::StringRef name) {
  return (llvm::Twine("field '") + name + "'").str();
}

std::string ErrorTextFieldValue::isVoid(llvm::StringRef label) {
  return (llvm::Twine("an expression of type 'void' cannot be the value of ") + label).str();
}

std::string ErrorTextFieldValue::undefined(llvm::StringRef label) {
  return (llvm::Twine("cannot infer the type of ") + label + " from 'undefined'").str();
}

std::string ErrorTextLiteralFits::noField(llvm::StringRef target, llvm::StringRef name) {
  return (llvm::Twine("the object literal does not fit '") + target + "': it has no field '" +
          name + "'; an object must have exactly the fields of its type")
      .str();
}

std::string ErrorTextLiteralFits::notMethod(llvm::StringRef target, llvm::StringRef name,
                                            llvm::StringRef type) {
  return (llvm::Twine("the object literal does not fit '") + target + "': '" + name +
          "' is a field of type '" + type + "' there, not a method")
      .str();
}

std::string ErrorTextLiteralFits::replaced(llvm::StringRef target, llvm::StringRef name) {
  return (llvm::Twine("the object literal does not fit '") + target + "': '" + name +
          "' is a field there that code may replace, "
          "which a method cannot be; write it as a field: '" +
          name + ": (...) => ...'")
      .str();
}

std::string ErrorTextLiteralFits::otherType(llvm::StringRef target, llvm::StringRef name,
                                            llvm::StringRef mine, llvm::StringRef theirs) {
  return (llvm::Twine("the object literal does not fit '") + target + "': '" + name + "' is '" +
          mine + "' here and '" + theirs + "' there")
      .str();
}

std::string ErrorTextLiteralMembers::missing(llvm::StringRef target, llvm::StringRef member) {
  return (llvm::Twine("the object literal does not fit '") + target + "': it has no '" + member +
          "'")
      .str();
}

std::string ErrorTextLiteralMembers::overloads(llvm::StringRef target, llvm::StringRef method) {
  return (llvm::Twine("the object literal does not fit '") + target + "': its method '" + method +
          "' has several signatures, and only a class implements those, "
          "each with a variant of its own")
      .str();
}

}
