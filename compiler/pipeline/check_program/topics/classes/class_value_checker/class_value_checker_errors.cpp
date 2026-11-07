#include "compiler/pipeline/check_program/topics/classes/class_value_checker/class_value_checker_errors.h"
#include "llvm/ADT/Twine.h"

namespace tz {

std::string ErrorTextConstructorType::typeArgs() {
  return "'Constructor' takes the type of the objects the class makes, e.g. "
         "'Constructor<User>', or nothing for any class";
}

std::string ErrorTextConstructorType::notObject(llvm::StringRef type) {
  return (llvm::Twine("a class makes objects, and '") + type +
          "' is not an object type; give the type of its objects, "
          "e.g. 'Constructor<User>'")
      .str();
}

std::string ErrorTextConstructorArgs::subclass() {
  return "'ConstructorArgs' is the list of the parameters of the constructor of the "
         "class extended, for the constructor of a class that extends another: "
         "'constructor(...args: ConstructorArgs) { super(...args) }'";
}

std::string ErrorTextConstructorArgs::several(llvm::StringRef parent) {
  return (llvm::Twine("'ConstructorArgs' is the list of the parameters of the constructor of '") +
          parent +
          "', which has several; write the parameters of the one to "
          "call")
      .str();
}

std::string ErrorTextConstructorArgs::later(llvm::StringRef parent) {
  return (llvm::Twine("'ConstructorArgs' needs the constructor of '") + parent +
          "', which is declared after this class; move '" + parent + "' up")
      .str();
}

std::string ErrorTextClassRefMember::prototype() {
  return "there are no prototypes in tz: the methods belong to the class "
         "itself, and a decorator gets the class; use it as it is";
}

std::string ErrorTextClassRefMember::runtime(llvm::StringRef type) {
  return (llvm::Twine("'") + type +
          "' is some class known only when the program "
          "runs, which has only its 'name'; make an object of it with 'new', "
          "or use reflection (docs/decorators.md, section 6)")
      .str();
}

std::string ErrorTextClassRefMember::nameOnly(llvm::StringRef name) {
  return (llvm::Twine("'") + name + "' as a value has only its 'name' in tz").str();
}

std::string ErrorTextValueNew::notClass(llvm::StringRef type) {
  return (llvm::Twine("'new' makes an object of a class, and this is a '") + type + "'").str();
}

std::string ErrorTextValueNew::valueOnly(llvm::StringRef name) {
  return (llvm::Twine("'new' of '") + name + "' as a value is not supported; write 'new " + name +
          "(...)' or a literal")
      .str();
}

std::string ErrorTextValueNew::typeArgs() {
  return "a class known only when the program runs takes no type "
         "arguments";
}

std::string ErrorTextValueNew::voidArg() {
  return "an expression of type 'void' has no value to pass";
}

}
