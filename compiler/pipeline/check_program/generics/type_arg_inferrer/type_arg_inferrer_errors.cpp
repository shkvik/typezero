#include "compiler/pipeline/check_program/generics/type_arg_inferrer/type_arg_inferrer_errors.h"
#include "llvm/ADT/Twine.h"

namespace tz {

std::string ErrorTextParamInferred::cannot(llvm::StringRef param, llvm::StringRef name, bool call,
                                           llvm::StringRef ints) {
  return (llvm::Twine("cannot infer type parameter '") + param + "' of '" + name + "': nothing " +
          (call ? "in the call" : "here") + " says what it is; give it, e.g. '" + name + "<" +
          ints + ">" + (call ? "(...)" : "") + "'")
      .str();
}

std::string ErrorTextIterableFits::elements(llvm::StringRef bound, llvm::StringRef given,
                                            llvm::StringRef wanted, llvm::StringRef name) {
  return (llvm::Twine("'") + bound + "' gives values of type '" + given +
          "', and the parameter 'Iterable<" + wanted + ">' of '" + name +
          "' takes values of type '" + wanted + "'")
      .str();
}

std::string ErrorTextIterableFits::notIter(llvm::StringRef bound, llvm::StringRef wanted,
                                           llvm::StringRef name) {
  return (llvm::Twine("'") + bound +
          "' cannot be iterated, and the parameter "
          "'Iterable<" +
          wanted + ">' of '" + name +
          "' takes something that can: an array, a string, a Map, a Set, an "
          "iterator, or an object with '[Symbol.iterator]()'")
      .str();
}

std::string ErrorTextConstraintMet::unsatisfied(llvm::StringRef bound, llvm::StringRef constraint,
                                                llvm::StringRef param, llvm::StringRef name) {
  return (llvm::Twine("type '") + bound + "' does not satisfy the constraint '" + constraint +
          "' of '" + param + "' in '" + name + "'")
      .str();
}

std::string ErrorTextTypeArgCount::tooMany(llvm::StringRef name, size_t count, size_t got) {
  return (llvm::Twine("'") + name + "' takes " + std::to_string(count) + " type argument" +
          (count == 1 ? "" : "s") + ", but got " + std::to_string(got))
      .str();
}

std::string ErrorTextCandidates::conflict(llvm::StringRef param, llvm::StringRef name,
                                          llvm::StringRef type, llvm::StringRef other) {
  return (llvm::Twine("type parameter '") + param + "' of '" + name + "' gets '" + type +
          "' from one argument and '" + other + "' from another; give it, e.g. '" + name + "<" +
          type + ">(...)'")
      .str();
}

std::string ErrorTextCallbackParam::param(llvm::StringRef name) {
  return (llvm::Twine("parameter '") + name + "' of this function").str();
}

std::string ErrorTextCallbackParam::params() {
  return "the parameters of this function";
}

std::string ErrorTextCallbackParam::uninferred(llvm::StringRef param, llvm::StringRef name,
                                               llvm::StringRef what, bool function) {
  return (llvm::Twine("cannot infer type parameter '") + param + "' of '" + name + "' for " + what +
          ": nothing else in the call says what it is; give it, e.g. '" + name + "<int>(...)'" +
          (function ? ", or the type of the parameter" : ""))
      .str();
}

std::string ErrorTextNotGrowing::grows(llvm::StringRef name, llvm::StringRef bound,
                                       llvm::StringRef param) {
  return (llvm::Twine("'") + name + "' calls itself with '" + bound + "' for '" + param +
          "', so each copy of it for some type needs another for a "
          "bigger one, without end; a generic function can call itself only with its own "
          "type parameters")
      .str();
}

}
