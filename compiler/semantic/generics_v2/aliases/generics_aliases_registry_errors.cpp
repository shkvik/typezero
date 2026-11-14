#include "compiler/semantic/generics_v2/aliases/generics_aliases_registry_errors.h"
#include "llvm/ADT/Twine.h"

namespace tz {

std::string ErrorTextAliasCount::tooMany(llvm::StringRef name, size_t count, size_t got) {
  return (llvm::Twine("'") + name + "' takes " + std::to_string(count) + " type argument" +
          (count == 1 ? "" : "s") + ", but got " + std::to_string(got))
      .str();
}

std::string ErrorTextAliasDefault::needs(llvm::StringRef name, size_t count) {
  return (llvm::Twine("'") + name + "' needs " + std::to_string(count) + " type argument" +
          (count == 1 ? "" : "s") + ", e.g. '" + name + "<int>'")
      .str();
}

std::string ErrorTextAliasMatch::unsatisfied(llvm::StringRef arg, llvm::StringRef constraint,
                                             llvm::StringRef param, llvm::StringRef name) {
  return (llvm::Twine("type '") + arg + "' does not satisfy the constraint '" + constraint +
          "' of '" + param + "' in '" + name + "'")
      .str();
}

std::string ErrorTextAliasSelf::itself(llvm::StringRef name) {
  return (llvm::Twine("type alias '") + name + "' refers to itself").str();
}

std::string ErrorTextAliasDepth::endless(llvm::StringRef name) {
  return (llvm::Twine("type alias '") + name +
          "' refers to itself with other type arguments, "
          "so it never ends")
      .str();
}

std::string ErrorTextAliasDepth::tailCalls(llvm::StringRef name, unsigned most) {
  return (llvm::Twine("type alias '") + name + "' calls itself more than " + std::to_string(most) +
          " times, so it may never end; "
          "TypeScript stops there too")
      .str();
}

std::string ErrorTextAliasDepth::nests(llvm::StringRef name, unsigned depth, unsigned calls) {
  return (llvm::Twine("type alias '") + name + "' nests more than " + std::to_string(depth) +
          " instances of itself, so it may never "
          "end; TypeScript stops there too, and a branch that is the alias "
          "itself, '... ? " +
          name + "<...> : ...', may go " + std::to_string(calls) + " deep")
      .str();
}

std::string ErrorTextAliasDepth::neededHere() {
  return "the instance needed here";
}

}
