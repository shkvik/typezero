#include "compiler/check/classes/instance_of_checker/instance_of_checker_errors.h"
#include "llvm/ADT/Twine.h"

namespace tz {

std::string ErrorTextInstanceOf::untypedMap(llvm::StringRef builtin, llvm::StringRef example) {
  return (llvm::Twine("'instanceof ") + builtin +
          "' cannot tell the types of the keys and values, which "
          "tz needs to use a " +
          builtin + "; keep it in a variable of its type, e.g. '" + example + "'")
      .str();
}

std::string ErrorTextInstanceOf::noneIs(llvm::StringRef base, llvm::StringRef what) {
  return (llvm::Twine("a '") + base + "' is never a " + what +
          ": none of its members is one, so this is always false")
      .str();
}

std::string ErrorTextInstanceOf::allAre(llvm::StringRef base, llvm::StringRef what,
                                        llvm::StringRef name) {
  return (llvm::Twine("every member of '") + base + "' is a " + what + ", so 'instanceof " + name +
          "' is always true")
      .str();
}

std::string ErrorTextInstanceOf::alwaysTrue(llvm::StringRef type, llvm::StringRef name) {
  return (llvm::Twine("the value is a '") + type + "', so 'instanceof " + name + "' is always true")
      .str();
}

std::string ErrorTextInstanceOf::orUndef(llvm::StringRef type) {
  return (llvm::Twine("the value is a '") + type + "' or undefined; test it with '!= undefined'")
      .str();
}

std::string ErrorTextInstanceOf::neverIs(llvm::StringRef type, llvm::StringRef what,
                                         llvm::StringRef name) {
  return (llvm::Twine("a '") + type + "' is never a " + what + ", so 'instanceof " + name +
          "' is always false")
      .str();
}

std::string ErrorTextInstanceOf::notObject(llvm::StringRef type) {
  return (llvm::Twine("'instanceof' tests an object, found '") + type +
          "', which is never an instance of a class")
      .str();
}

std::string ErrorTextInstanceOf::notClass() {
  return "the right side of 'instanceof' must be a class";
}

std::string ErrorTextInstanceOf::cantTell(llvm::StringRef generic, llvm::StringRef base,
                                          llvm::StringRef hint) {
  return (llvm::Twine("'instanceof ") + generic + "' cannot tell which '" + generic + "' a '" +
          base +
          "' would be: each list of type "
          "arguments of '" +
          generic + "' makes a class of its own; " + hint)
      .str();
}

std::string ErrorTextInstanceOf::extendHint(llvm::StringRef generic) {
  return (llvm::Twine("test for a class that extends it, e.g. 'x instanceof Bus' with 'class Bus "
                      "extends ") +
          generic + " {}'")
      .str();
}

std::string ErrorTextInstanceOf::unionHint(llvm::StringRef generic, llvm::StringRef other) {
  return (llvm::Twine("keep the value in a variable of a union of the classes it may be, e.g. '") +
          generic + "<int> | " + other + "'")
      .str();
}

std::string ErrorTextInstanceOf::neverThat(llvm::StringRef base, llvm::StringRef cls) {
  return (llvm::Twine("a '") + base + "' is never a '" + cls + "', so this is always false").str();
}

std::string ErrorTextInstanceOf::neverUnion(llvm::StringRef base, llvm::StringRef cls) {
  return (llvm::Twine("a '") + base + "' is never a '" + cls +
          "': none of its members is or extends it, so this is always "
          "false")
      .str();
}

std::string ErrorTextInstanceOf::neverClass(llvm::StringRef base, llvm::StringRef cls) {
  return (llvm::Twine("a '") + base + "' is never a '" + cls +
          "': classes are told apart by their names, and neither extends "
          "the other, so this is always false")
      .str();
}

std::string ErrorTextInstanceOf::notImpl(llvm::StringRef base, llvm::StringRef cls) {
  return (llvm::Twine("a '") + base + "' is never a '" + cls + "': '" + cls +
          "' does not implement it, so this is always false")
      .str();
}

std::string ErrorTextInstanceOf::neverMade(llvm::StringRef base, llvm::StringRef cls) {
  return (llvm::Twine("an object of type '") + base + "' is never a '" + cls + "': only 'new " +
          cls + "(...)' creates one, so this is always false")
      .str();
}

}
