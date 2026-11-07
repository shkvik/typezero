#include "compiler/check/classes/inherit_checker/inherit_checker_errors.h"
#include "llvm/ADT/Twine.h"

namespace tz {

std::string ErrorTextAbstract::needsClass(llvm::StringRef fn, llvm::StringRef cls) {
  return (llvm::Twine("abstract method '") + fn + "' needs an abstract class: '" + cls +
          "' can be created with 'new', and the method would "
          "have no body; write 'abstract class " +
          cls + "'")
      .str();
}

std::string ErrorTextAbstract::notImpl(llvm::StringRef cls, llvm::StringRef variant,
                                       llvm::StringRef owner) {
  return (llvm::Twine("class '") + cls + "' does not implement the abstract method " + variant +
          " of '" + owner + "'; implement it, or make '" + cls + "' abstract too")
      .str();
}

std::string ErrorTextImplements::prefix(llvm::StringRef cls, llvm::StringRef iface) {
  return (llvm::Twine("class '") + cls + "' implements '" + iface + "'").str();
}

std::string ErrorTextImplements::fieldType(llvm::StringRef prefix, llvm::StringRef name,
                                           llvm::StringRef type, llvm::StringRef cls,
                                           llvm::StringRef actual) {
  return (llvm::Twine(prefix) + ", whose field '" + name + "' is '" + type + "', but in '" + cls +
          "' it is '" + actual +
          "'; the types must be the same, or objects "
          "of a type down the chain of the interface's, such as 'Dog' for "
          "'Animal'")
      .str();
}

std::string ErrorTextImplements::noMember(llvm::StringRef prefix, llvm::StringRef name,
                                          bool optional, llvm::StringRef type) {
  return (llvm::Twine(prefix) + ", but has no '" + name + "'; declare it, e.g. '" + name +
          (optional ? "?" : "") + ": " + type + "'")
      .str();
}

std::string ErrorTextImplements::fieldNotFn(llvm::StringRef name, llvm::StringRef type,
                                            llvm::StringRef iface, llvm::StringRef cls) {
  return (llvm::Twine("'") + name + "' is a field of type '" + type + "' in '" + iface +
          "', which '" + cls + "' implements, so it cannot be a method")
      .str();
}

std::string ErrorTextImplements::mustField(llvm::StringRef name, llvm::StringRef iface,
                                           llvm::StringRef cls) {
  return (llvm::Twine("'") + name + "' is a field in '" + iface +
          "' that code may replace, so in '" + cls +
          "', which implements it, it must be a field too: '" + name +
          " = (...) => ...'; or declare it in '" + iface + "' as a method, '" + name +
          "(...): ...'")
      .str();
}

std::string ErrorTextImplements::returns(llvm::StringRef name, llvm::StringRef ret,
                                         llvm::StringRef iface, llvm::StringRef cls,
                                         llvm::StringRef says) {
  return (llvm::Twine("'") + name + "' returns '" + ret + "', and '" + iface + "', which '" + cls +
          "' implements, says it returns '" + says + "'")
      .str();
}

std::string ErrorTextImplements::params(llvm::StringRef prefix, llvm::StringRef name,
                                        llvm::StringRef type, llvm::StringRef cls,
                                        llvm::StringRef variants) {
  return (llvm::Twine(prefix) + ", whose '" + name + "' is '" + type + "', but '" + cls + "." +
          name + "' takes other parameters: " + variants)
      .str();
}

std::string ErrorTextImplements::noVariant(llvm::StringRef prefix, llvm::StringRef name,
                                           llvm::StringRef signature) {
  return (llvm::Twine(prefix) + ", but has no variant of '" + name + "' for its signature '" +
          signature + "'; each signature needs a variant with these parameters")
      .str();
}

std::string ErrorTextSuperCall::outside() {
  return "'super(...)' can be called only in the constructor of a class that "
         "extends another";
}

std::string ErrorTextSuperCall::noParent(llvm::StringRef cls) {
  return (llvm::Twine("'super(...)' calls the constructor of the class that '") + cls +
          "' extends, and it extends none")
      .str();
}

std::string ErrorTextSuperCall::statement() {
  return "'super(...)' must be a statement of its own in the body of the "
         "constructor, not inside a block, a condition, a loop or another "
         "expression: the fields are set right after it";
}

std::string ErrorTextSuperCall::twice(llvm::StringRef parent) {
  return (llvm::Twine("'super(...)' is called a second time; the constructor of '") + parent +
          "' runs once")
      .str();
}

std::string ErrorTextSuperCall::noArgs(llvm::StringRef parent) {
  return (llvm::Twine("class '") + parent +
          "' has no constructor, so "
          "'super()' takes no arguments")
      .str();
}

std::string ErrorTextSuperMethod::outside() {
  return "'super' is the class that the class of the method "
         "extends; it exists in the methods of a class that "
         "extends another";
}

std::string ErrorTextSuperMethod::early(llvm::StringRef name, llvm::StringRef field) {
  return (llvm::Twine("'super.") + name + "()' is called before every field is set: field '" +
          field + "' is not set yet, and the method may read it")
      .str();
}

std::string ErrorTextSuperMethod::isField(llvm::StringRef name, llvm::StringRef parent) {
  return (llvm::Twine("'") + name + "' is a field of '" + parent +
          "'; call the function it holds as 'this." + name + "(...)'")
      .str();
}

std::string ErrorTextSuperMethod::noMethod(llvm::StringRef parent, llvm::StringRef name) {
  return (llvm::Twine("class '") + parent + "' has no method '" + name + "'").str();
}

std::string ErrorTextSuperMethod::abstract(llvm::StringRef name, llvm::StringRef owner) {
  return (llvm::Twine("'") + name + "' is abstract in '" + owner + "': it has no body to call")
      .str();
}

}
