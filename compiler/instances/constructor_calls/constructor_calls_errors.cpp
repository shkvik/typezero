#include "compiler/instances/constructor_calls/constructor_calls_errors.h"
#include "llvm/ADT/Twine.h"

namespace tz {

std::string ErrorTextOverriddenCall::overridden(llvm::StringRef caller, llvm::StringRef method,
                                                llvm::StringRef path, llvm::StringRef child) {
  return (llvm::Twine(caller) + " calls '" + method + "'" + path + ", which '" + child +
          "' overrides: it would run before the fields of '" + child +
          "' are set; call it once the object is made")
      .str();
}

std::string ErrorTextOverriddenCall::ctorCall(llvm::StringRef className, llvm::StringRef method,
                                              llvm::StringRef child) {
  std::string caller = (llvm::Twine("the constructor of '") + className + "'").str();

  return ErrorTextOverriddenCall::overridden(caller, method, "", child);
}

std::string ErrorTextOverriddenCall::ctorReach(llvm::StringRef className, llvm::StringRef method,
                                               llvm::StringRef child) {
  std::string caller = (llvm::Twine("the constructor of '") + className + "'").str();

  return ErrorTextOverriddenCall::overridden(caller, method, " (through the methods it calls)",
                                             child);
}

std::string ErrorTextOverriddenCall::fieldsCall(llvm::StringRef className, llvm::StringRef method,
                                                llvm::StringRef child) {
  std::string caller = (llvm::Twine("the initializers of the fields of '") + className + "'").str();

  return ErrorTextOverriddenCall::overridden(caller, method, "", child);
}

std::string ErrorTextOverriddenCall::fieldsReach(llvm::StringRef className, llvm::StringRef method,
                                                 llvm::StringRef child) {
  std::string caller = (llvm::Twine("the initializers of the fields of '") + className + "'").str();

  return ErrorTextOverriddenCall::overridden(caller, method, " (through the methods it calls)",
                                             child);
}

}
