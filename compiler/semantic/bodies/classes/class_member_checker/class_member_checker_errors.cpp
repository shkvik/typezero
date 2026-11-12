#include "compiler/semantic/bodies/classes/class_member_checker/class_member_checker_errors.h"
#include "llvm/ADT/Twine.h"

namespace tz {

std::string ErrorTextAccess::elsewhere(llvm::StringRef name, llvm::StringRef cls,
                                       llvm::StringRef current, llvm::StringRef other) {
  return (llvm::Twine("'") + name + "' is protected in class '" + cls + "'; '" + current +
          "' may use it only on its own objects, and this one is of class '" + other + "'")
      .str();
}

std::string ErrorTextAccess::hiddenPrivate(llvm::StringRef name, llvm::StringRef word,
                                           llvm::StringRef cls) {
  return (llvm::Twine("'") + name + "' is " + word + " in class '" + cls + "'").str();
}

std::string ErrorTextAccess::hiddenProtected(llvm::StringRef name, llvm::StringRef word,
                                             llvm::StringRef cls) {
  auto message = ErrorTextAccess::hiddenPrivate(name, word, cls);

  return message +
         (llvm::Twine("; only '") + cls + "' and the classes that extend it can use it").str();
}

std::string ErrorTextClassField::readEarly(llvm::StringRef name) {
  return (llvm::Twine("field '") + name + "' is read before it is set").str();
}

std::string ErrorTextClassField::laterType(llvm::StringRef name) {
  return (llvm::Twine("the type of field '") + name +
          "' comes from its initializer, which runs later; declare "
          "it: '" +
          name + ": T = ...'")
      .str();
}

std::string ErrorTextClassField::isStatic(llvm::StringRef name, llvm::StringRef cls) {
  return (llvm::Twine("'") + name + "' is a static member of '" + cls +
          "': use it through the class, '" + cls + "." + name + "'")
      .str();
}

std::string ErrorTextClassField::noProperty(llvm::StringRef name, llvm::StringRef type) {
  return (llvm::Twine("property '") + name + "' does not exist on type '" + type + "'").str();
}

std::string ErrorTextClassField::earlyThis(llvm::StringRef name, llvm::StringRef field) {
  return (llvm::Twine("'this.") + name + "' is used before every field is set: field '" + field +
          "' is not set yet")
      .str();
}

std::string ErrorTextClassField::genericVal(llvm::StringRef name) {
  return (llvm::Twine("'") + name +
          "' is a generic method, so it can only be called: each "
          "call runs a copy of it made for its types; wrap it in an arrow, e.g. "
          "'(x) => o." +
          name + "(x)'")
      .str();
}

std::string ErrorTextFieldTarget::statsDate(llvm::StringRef name) {
  return (llvm::Twine("'") + name + "' cannot be assigned: it is made from '" + name +
          "Ms' when it is read; keep the date in a variable of its own")
      .str();
}

std::string ErrorTextFieldTarget::setter(llvm::StringRef name, llvm::StringRef type,
                                         llvm::StringRef setter) {
  return (llvm::Twine("'") + name + "' is a method of '" + type + "'; change it with '" + setter +
          "(...)'")
      .str();
}

std::string ErrorTextFieldTarget::library(llvm::StringRef name, llvm::StringRef type) {
  return (llvm::Twine("'") + name + "' is a method of '" + type +
          "' and cannot be assigned: the object changes "
          "what it gives itself")
      .str();
}

std::string ErrorTextFieldTarget::method(llvm::StringRef name, llvm::StringRef type) {
  return (llvm::Twine("'") + name + "' is a method of '" + type +
          "' and cannot be replaced: a method belongs to the class, "
          "not to the object; to change what it does, declare a "
          "field that holds a function: '" +
          name + ": () => void = () => { ... }'")
      .str();
}

std::string ErrorTextFieldTarget::readOnly(llvm::StringRef name, llvm::StringRef type) {
  return (llvm::Twine("cannot assign to '") + name + "' because it is a read-only field of '" +
          type + "'; only its constructor can set it")
      .str();
}

}
