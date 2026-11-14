#include "compiler/semantic/code/returns/code_returns_registry_errors.h"
#include "llvm/ADT/Twine.h"

namespace tz {

std::string ErrorTextReturnPlace::outside() {
  return "'return' can only be used inside a function";
}

std::string ErrorTextConstructorReturn::value() {
  return "a constructor cannot return a value: 'new' gives the object";
}

std::string ErrorTextConstructorReturn::early() {
  return "the constructor returns before 'super(...)', which makes the "
         "object (JavaScript throws a ReferenceError there)";
}

std::string ErrorTextReturnAwait::concise() {
  return "'await' here changes nothing: an async function waits for the "
         "Promise it returns; leave out 'await'";
}

std::string ErrorTextReturnAwait::returned() {
  return "'return await' changes nothing: an async function waits for the "
         "Promise it returns; write 'return' without 'await'";
}

std::string ErrorTextReturnValue::missing(llvm::StringRef label, llvm::StringRef type) {
  return (llvm::Twine(label) + " must return a value of type '" + type + "'").str();
}

std::string ErrorTextReturnValue::wrong(llvm::StringRef label, llvm::StringRef type,
                                        llvm::StringRef given) {
  return (llvm::Twine(label) + " returns '" + type + "', found '" + given + "'").str();
}

std::string ErrorTextReturnValue::promise(llvm::StringRef given, llvm::StringRef label,
                                          llvm::StringRef type) {
  return (llvm::Twine("this Promise gives '") + given + "', but " + label + " gives '" + type +
          "'; await it and convert the value")
      .str();
}

std::string ErrorTextReturnValue::notVoid(llvm::StringRef label, llvm::StringRef type) {
  return (llvm::Twine(label) +
          " is declared to return 'void', "
          "but returns '" +
          type + "'")
      .str();
}

}
