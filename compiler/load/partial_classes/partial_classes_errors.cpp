#include "compiler/load/partial_classes/partial_classes_errors.h"
#include "llvm/ADT/Twine.h"

namespace tz {

std::string ErrorTextListHasPart::noPart(llvm::StringRef path) {
  return (llvm::Twine("an import must name what it takes: 'import \"") + path +
          "\"' only lists a file with a part of a class declared here ('partial class'); "
          "import the names you need, e.g. 'import { a } from \"" +
          path + "\"'")
      .str();
}

std::string ErrorTextPartClass::notImported(llvm::StringRef name) {
  return (llvm::Twine("'partial class ") + name +
          "' adds methods to a class of another file: import the class from the file that "
          "declares it, e.g. 'import { " +
          name + " } from \"./" + name.lower() + "\"'")
      .str();
}

std::string ErrorTextPartClass::library(llvm::StringRef name, llvm::StringRef path) {
  return (llvm::Twine("'partial class ") + name + "': \"" + path +
          "\" is a module of the library; parts are written only for classes of the program")
      .str();
}

std::string ErrorTextPartClass::undeclared(llvm::StringRef name, llvm::StringRef path) {
  return (llvm::Twine("'partial class ") + name + "': \"" + path + "\" does not declare a class '" +
          name + "'; a part is written for a class declared in the file it is imported from")
      .str();
}

std::string ErrorTextPartClass::notListed(llvm::StringRef name, llvm::StringRef file,
                                          llvm::StringRef path) {
  return (llvm::Twine("'partial class ") + name + "': " + file +
          " does not list this file among the parts of '" + name + "'; add 'import \"" + path +
          "\"' to " + file)
      .str();
}

std::string ErrorTextPartClass::fromValue(llvm::StringRef name) {
  return (llvm::Twine("'partial class ") + name + "': class '" + name +
          "' extends a value; parts are written for a class that extends a class by name")
      .str();
}

std::string ErrorTextPartShape::field(llvm::StringRef name, llvm::StringRef field) {
  return (llvm::Twine("'partial class ") + name + "' has only methods: declare the field '" +
          field + "' where '" + name + "' is declared")
      .str();
}

std::string ErrorTextPartShape::ctor(llvm::StringRef name) {
  return (llvm::Twine("'partial class ") + name +
          "' has only methods: write the constructor where '" + name + "' is declared")
      .str();
}

std::string ErrorTextPartShape::header(llvm::StringRef name) {
  return (llvm::Twine("'partial class ") + name +
          "' takes 'extends' and 'implements' from the class: write them where '" + name +
          "' is declared")
      .str();
}

std::string ErrorTextPartShape::decorators(llvm::StringRef name) {
  return (llvm::Twine("'partial class ") + name +
          "' takes its decorators from the class: write them where '" + name + "' is declared")
      .str();
}

std::string ErrorTextPartShape::typeParams(llvm::StringRef name, llvm::StringRef params) {
  return (llvm::Twine("a part repeats the type parameters of the class by name: write "
                      "'partial class ") +
          name + params + "'")
      .str();
}

std::string ErrorTextMethodIsNew::twice(llvm::StringRef method, llvm::StringRef name,
                                        llvm::StringRef file) {
  return (llvm::Twine("method '") + method + "' of '" + name + "' is already declared in " + file +
          ": a method has one name in the whole class, and its overloads stay in one file")
      .str();
}

}
