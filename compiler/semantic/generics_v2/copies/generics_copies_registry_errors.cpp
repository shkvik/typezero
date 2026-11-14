#include "compiler/semantic/generics_v2/copies/generics_copies_registry_errors.h"
#include "llvm/ADT/Twine.h"

namespace tz {

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

std::string ErrorTextInstanceUse::cannotUse(llvm::StringRef name, llvm::StringRef bindings,
                                            llvm::StringRef message) {
  return (llvm::Twine("cannot use '") + name + "' with " + bindings + ": " + message).str();
}

std::string ErrorTextInstanceUse::usesHere(llvm::StringRef name, llvm::StringRef next) {
  return (llvm::Twine("in '") + name + "', which uses '" + next + "' here").str();
}

std::string ErrorTextInstanceUse::here(llvm::StringRef name) {
  return (llvm::Twine("in '") + name + "', here").str();
}

}
