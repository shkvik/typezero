#include "compiler/semantic/infer/late_classes/late_classes_errors.h"
#include "llvm/ADT/Twine.h"

namespace tz {

std::string ErrorTextBaseClass::atRun(llvm::StringRef type) {
  return (llvm::Twine("the class extended must be known when compiling: the fields of an object "
                      "lie at places fixed then, and '") +
          type +
          "' is some class known only when the program runs (docs/decorators.md, section 5); "
          "extend a class by its name, or take it in a generic function, 'function Mixin<B "
          "extends Constructor>(Base: B)'")
      .str();
}

std::string ErrorTextBaseClass::notClass(llvm::StringRef type) {
  return (llvm::Twine("a class can extend only a class, and this is a '") + type + "'").str();
}

std::string ErrorTextBaseClass::javascript(llvm::StringRef className) {
  return (llvm::Twine("a class cannot extend '") + className +
          "' of JavaScript; extend a class of the program")
      .str();
}

std::string ErrorTextBaseClass::generic(llvm::StringRef className) {
  return (llvm::Twine("'") + className + "' is generic; extend one of its instances, e.g. '" +
          className + "<int>'")
      .str();
}

std::string ErrorTextBaseClass::madeByFs(llvm::StringRef className) {
  return (llvm::Twine("a class cannot extend '") + className +
          "': only the functions of node:fs make its objects")
      .str();
}

std::string ErrorTextStaticType::unknown(llvm::StringRef field) {
  return (llvm::Twine("cannot infer the type of static field '") + field +
          "' from this value; declare it, e.g. 'static " + field + ": int | undefined = undefined'")
      .str();
}

std::string ErrorTextOldObject::held(llvm::StringRef className, llvm::StringRef field) {
  return (llvm::Twine("the decorator gives '") + className +
          "' another class in its place, and static field '" + field +
          "' holds an object made before, of the class as declared; make that object where it "
          "is used, or keep it outside the class")
      .str();
}

std::string ErrorTextOldObject::decorator() {
  return "the decorator is here";
}

std::string ErrorTextReplacedField::clash(llvm::StringRef child, llvm::StringRef className,
                                          llvm::StringRef field) {
  return (llvm::Twine("'") + child + "' extends '" + className +
          "', whose decorator gives a class with a field '" + field +
          "' of its own in its place; give this field another name")
      .str();
}

std::string ErrorTextStaticType::fieldLabel(llvm::StringRef name) {
  return (llvm::Twine("static field '") + name + "'").str();
}

}
