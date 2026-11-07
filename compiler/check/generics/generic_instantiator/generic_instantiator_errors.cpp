#include "compiler/check/generics/generic_instantiator/generic_instantiator_errors.h"
#include "llvm/ADT/Twine.h"

namespace tz {

std::string ErrorTextGenericCount::tooMany(llvm::StringRef name, size_t count, size_t got) {
  return (llvm::Twine("'") + name + "' takes " + std::to_string(count) + " type argument" +
          (count == 1 ? "" : "s") + ", but got " + std::to_string(got))
      .str();
}

std::string ErrorTextGenericValue::needsArgs(llvm::StringRef name) {
  return (llvm::Twine("'") + name +
          "' is generic, so a value of it needs its type arguments, "
          "e.g. '" +
          name + "<int>', or a function type where it goes")
      .str();
}

std::string ErrorTextOwnClass::literal(llvm::StringRef name) {
  return (llvm::Twine("generic function '") + name +
          "' cannot return an object literal with methods yet: each copy of "
          "it makes a class of its own; declare a class, or give the "
          "object function fields, 'f: () => ...'")
      .str();
}

std::string ErrorTextInstanceChain::endless(llvm::StringRef name, llvm::StringRef types) {
  return (llvm::Twine("calling '") + name +
          "' here needs another copy of it for each copy of the "
          "functions that lead here, without end: its type arguments keep growing ('" +
          types +
          "' by now); generic functions that call each other must pass "
          "their type parameters on unchanged")
      .str();
}

std::string ErrorTextInstanceParams::meaningless(llvm::StringRef name, llvm::StringRef bindings,
                                                 llvm::StringRef param) {
  return (llvm::Twine("cannot call '") + name + "' with " + bindings +
          ": the types of its parameters, '" + param + "', mean nothing for these types")
      .str();
}

std::string ErrorTextInstanceCall::cannotCall(llvm::StringRef name, llvm::StringRef bindings,
                                              llvm::StringRef message) {
  return (llvm::Twine("cannot call '") + name + "' with " + bindings + ": " + message).str();
}

std::string ErrorTextInstanceCall::inWhich(llvm::StringRef name, bool uses, llvm::StringRef next) {
  return (llvm::Twine("in '") + name + "', which " + (uses ? "uses '" : "calls '") + next +
          "' here")
      .str();
}

std::string ErrorTextInstanceCall::here(llvm::StringRef name) {
  return (llvm::Twine("in '") + name + "', here").str();
}

}
