#include "compiler/pipeline/check_program/generics/generic_class_instantiator/generic_class_instantiator_errors.h"
#include "llvm/ADT/Twine.h"

namespace tz {

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
  std::string ints;
  for (size_t i = 0; i < count; ++i) {
    ints += i == 0 ? "int" : ", int";
  }

  return (llvm::Twine("'") + name + "' needs " + std::to_string(count) + " type argument" +
          (count == 1 ? "" : "s") + ", e.g. '" + name + "<" + ints + ">'")
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
