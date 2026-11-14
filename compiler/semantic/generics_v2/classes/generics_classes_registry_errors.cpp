#include "compiler/semantic/generics_v2/classes/generics_classes_registry_errors.h"
#include "llvm/ADT/Twine.h"

namespace tz {

std::string ErrorTextDeclaredFirst::before(llvm::StringRef name) {
  return (llvm::Twine("class '") + name +
          "' is used before its declaration, which JavaScript has not run "
          "yet here (a ReferenceError); move the class up")
      .str();
}

std::string ErrorTextConcrete::abstract(llvm::StringRef name) {
  return (llvm::Twine("class '") + name +
          "' is abstract and cannot be created: create "
          "an object of a class that extends it")
      .str();
}

std::string ErrorTextConstructorFit::inferArgs(llvm::StringRef name, bool none, size_t count) {
  return (llvm::Twine("cannot infer the type arguments of '") + name + "': " +
          (none ? "no constructor takes " : "several constructors take ") + std::to_string(count) +
          " argument" + (count == 1 ? "" : "s") + "; give them, e.g. 'new " + name + "<int>(...)'")
      .str();
}

std::string ErrorTextClassInstance::tooMany(llvm::StringRef name, size_t count, size_t got) {
  return (llvm::Twine("'") + name + "' takes " + std::to_string(count) + " type argument" +
          (count == 1 ? "" : "s") + ", but got " + std::to_string(got))
      .str();
}

std::string ErrorTextClassInstance::eventMap() {
  return "'EventEmitter' needs its event map here, e.g. 'EventEmitter<{ data: "
         "[chunk: string]; end: [] }>', or the type of an emitter, e.g. 'typeof "
         "bus'";
}

std::string ErrorTextClassInstance::needs(llvm::StringRef name, size_t count) {
  auto ints = std::string();
  for (size_t i = 0; i < count; ++i) {
    ints += i == 0 ? "int" : ", int";
  }

  return (llvm::Twine("'") + name + "' needs " + std::to_string(count) + " type argument" +
          (count == 1 ? "" : "s") + ", e.g. '" + name + "<" + ints + ">'")
      .str();
}

std::string ErrorTextClassInstance::chunks(llvm::StringRef name) {
  return (llvm::Twine("'") + name + "' needs the type of its chunks, e.g. '" + name +
          "<Buffer>', '" + name + "<string>' or '" + name + "<User>'")
      .str();
}

std::string ErrorTextClassInstance::undefinedArg(llvm::StringRef arg, llvm::StringRef name) {
  return (llvm::Twine("'") + arg + "' cannot be a type argument of '" + name + "'").str();
}

std::string ErrorTextClassInstance::unsatisfied(llvm::StringRef arg, llvm::StringRef constraint,
                                                llvm::StringRef param, llvm::StringRef name) {
  return (llvm::Twine("type '") + arg + "' does not satisfy the constraint '" + constraint +
          "' of '" + param + "' in '" + name + "'")
      .str();
}

std::string ErrorTextClassInstance::endless(llvm::StringRef name) {
  return (llvm::Twine("generic class '") + name +
          "' uses itself with type arguments that keep "
          "growing, so each instance of it needs another, without end; a "
          "generic class can use itself only with its own type parameters")
      .str();
}

}
