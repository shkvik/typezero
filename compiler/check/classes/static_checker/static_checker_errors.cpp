#include "compiler/check/classes/static_checker/static_checker_errors.h"
#include "llvm/ADT/Twine.h"

namespace tz {

std::string ErrorTextStaticOrder::usedEarly(llvm::StringRef var, llvm::StringRef cls) {
  return (llvm::Twine("'") + var + "' is used before class '" + cls +
          "' is declared, which sets it")
      .str();
}

std::string ErrorTextStaticOrder::beforeSet(llvm::StringRef var) {
  return (llvm::Twine("'") + var +
          "' is read before it is set: the static "
          "fields are set in the order they are declared")
      .str();
}

std::string ErrorTextStaticLater::typeOf(llvm::StringRef var, llvm::StringRef field) {
  return (llvm::Twine("the type of '") + var +
          "' comes from its value, which is set later; declare it: "
          "'static " +
          field + ": T = ...'")
      .str();
}

std::string ErrorTextStaticMember::instance(llvm::StringRef name, llvm::StringRef cls) {
  return (llvm::Twine("'") + name + "' is a member of the instances of '" + cls +
          "', not of the class: create one with 'new " + cls + "(...)'")
      .str();
}

std::string ErrorTextStaticMember::noMember(llvm::StringRef cls, llvm::StringRef name) {
  return (llvm::Twine("class '") + cls + "' has no static member '" + name + "'").str();
}

std::string ErrorTextStaticField::isMethod(llvm::StringRef name, llvm::StringRef cls) {
  return (llvm::Twine("'") + name + "' is a static method of '" + cls + "' and cannot be replaced")
      .str();
}

std::string ErrorTextStaticField::noField(llvm::StringRef cls, llvm::StringRef name) {
  return (llvm::Twine("class '") + cls + "' has no static field '" + name + "'").str();
}

std::string ErrorTextStaticField::readOnly(llvm::StringRef var) {
  return (llvm::Twine("cannot assign to '") + var + "' because it is a read-only static field")
      .str();
}

std::string ErrorTextStaticCall::notFunc(llvm::StringRef cls, llvm::StringRef name,
                                         llvm::StringRef type) {
  return (llvm::Twine("'") + cls + "." + name + "' is not a function; it has type '" + type + "'")
      .str();
}

}
