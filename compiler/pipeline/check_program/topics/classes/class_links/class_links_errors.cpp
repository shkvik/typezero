#include "compiler/pipeline/check_program/topics/classes/class_links/class_links_errors.h"
#include "llvm/ADT/Twine.h"

namespace tz {

std::string ErrorTextNotAField::fieldOf(llvm::StringRef name, llvm::StringRef owner) {
  return (llvm::Twine("'") + name + "' is a field of '" + owner +
          "', so a class that extends it cannot have a method of that "
          "name")
      .str();
}

std::string ErrorTextOverrideTarget::noParent(llvm::StringRef name, llvm::StringRef cls) {
  return (llvm::Twine("'override' says that '") + name + "' replaces a method of the class that '" +
          cls + "' extends, and it extends none")
      .str();
}

std::string ErrorTextOverrideTarget::noMethod(llvm::StringRef name, llvm::StringRef parent) {
  return (llvm::Twine("'") + name + "' is marked 'override', but '" + parent + "' has no method '" +
          name + "'")
      .str();
}

std::string ErrorTextOverrideTarget::noParams(llvm::StringRef name, llvm::StringRef parent,
                                              bool one, llvm::StringRef list) {
  return (llvm::Twine("'") + name + "' is marked 'override', but no method of '" + parent +
          "' has its parameters: there " + (one ? "is " : "are ") + list +
          "; an override takes the same parameters")
      .str();
}

std::string ErrorTextArrowOverride::arrow(llvm::StringRef name, llvm::StringRef base) {
  return (llvm::Twine("'") + name + "' is an arrow in a field of '" + base +
          "', which each object holds itself in JavaScript, so a method of "
          "a class that extends it would never run; override it with a "
          "field too: '" +
          name + " = (...) => ...'")
      .str();
}

std::string ErrorTextBaseVisible::hidden(llvm::StringRef name, llvm::StringRef base) {
  return (llvm::Twine("'") + name + "' is private in class '" + base +
          "', so a class that extends it cannot override it")
      .str();
}

std::string ErrorTextAccessKept::narrower(llvm::StringRef name, llvm::StringRef baseAccess,
                                          llvm::StringRef base, llvm::StringRef access) {
  return (llvm::Twine("'") + name + "' is " + baseAccess + " in '" + base +
          "', so its override cannot be " + access)
      .str();
}

std::string ErrorTextBodyKept::again(llvm::StringRef name, llvm::StringRef base,
                                     llvm::StringRef cls) {
  return (llvm::Twine("'") + name + "' has a body in '" + base +
          "', so it cannot be abstract again in '" + cls + "'")
      .str();
}

std::string ErrorTextSameTypeArgs::typeArgs(llvm::StringRef name, llvm::StringRef type,
                                            llvm::StringRef baseType, llvm::StringRef baseName,
                                            llvm::StringRef baseClass) {
  return (llvm::Twine("'") + name + "' of '" + type + "' overrides '" + baseType + "." + baseName +
          "' only with these type arguments, which is not supported; give "
          "the method the same parameters in '" +
          baseClass + "'")
      .str();
}

std::string ErrorTextInheritedReturn::returns(llvm::StringRef name, llvm::StringRef base,
                                              llvm::StringRef type) {
  return (llvm::Twine("'") + name + "' overrides '" + base + "." + name + "', so it returns '" +
          type +
          "' too; to return another type, "
          "declare it in '" +
          base + "'")
      .str();
}

}
