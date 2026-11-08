#include "compiler/semantic/bodies/top_level_checker/top_level_checker_errors.h"
#include "llvm/ADT/Twine.h"

namespace tz {

std::string ErrorTextNotLoaded::whileLoads(llvm::StringRef reader, llvm::StringRef what,
                                           llvm::StringRef source, llvm::StringRef chain,
                                           llvm::StringRef declared) {
  return (llvm::Twine(reader) + " reads " + what + " while it loads, but " + source +
          " is not loaded yet: " + chain + "; compute the value inside a function, or move '" +
          declared + "' to a file outside the cycle")
      .str();
}

std::string ErrorTextNotLoaded::beforeCall(llvm::StringRef what, llvm::StringRef reader,
                                           llvm::StringRef early) {
  return (llvm::Twine(what) + " has no type yet when " + reader + " first calls '" + early +
          "', which needs to know what '" + early +
          "' returns: the files import each other; declare the return type of '" + early + "'")
      .str();
}

std::string ErrorTextNotLoaded::inFunction(llvm::StringRef what, llvm::StringRef reader) {
  return (llvm::Twine(what) + " has no type yet where " + reader +
          " creates this function: the files import each other; declare the "
          "function with 'function' at the top level of the file, which is "
          "checked after every file")
      .str();
}

std::string ErrorTextNotLoaded::label(llvm::StringRef name, llvm::StringRef declared,
                                      llvm::StringRef file) {
  return (llvm::Twine("'") + name + "' ('" + declared + "' of " + file + ")").str();
}

std::string ErrorTextNotLoaded::labelOf(llvm::StringRef name, llvm::StringRef file) {
  return (llvm::Twine("'") + name + "' of " + file).str();
}

std::string ErrorTextNotLoaded::quoted(llvm::StringRef name) {
  return (llvm::Twine("'") + name + "'").str();
}

std::string ErrorTextImportAssign::imported(llvm::StringRef name) {
  return (llvm::Twine("cannot assign to '") + name +
          "' because it is imported; change it in "
          "the file that declares it, e.g. with a "
          "function that file exports")
      .str();
}

std::string ErrorTextNotLoaded::pending(llvm::StringRef name, bool started) {
  return (llvm::Twine(" -> (") + name + (started ? " already started)" : " not loaded yet)")).str();
}

}
