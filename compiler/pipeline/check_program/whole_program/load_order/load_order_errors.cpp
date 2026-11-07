#include "compiler/pipeline/check_program/whole_program/load_order/load_order_errors.h"
#include "compiler/pipeline/check_program/check_state/check_state.h"
#include "compiler/pipeline/check_program/topics/functions/function_checker/function_checker_errors.h"
#include "llvm/ADT/Twine.h"

namespace tz {

std::string ErrorTextEarlyRead::sameFile(llvm::StringRef name) {
  return (llvm::Twine("'") + name + "' is used before it is declared").str();
}

std::string ErrorTextEarlyRead::label(llvm::StringRef name, llvm::StringRef var,
                                      llvm::StringRef file) {
  if (name != var) {
    return (llvm::Twine("'") + name + "' ('" + var + "' of " + file + ")").str();
  }

  return (llvm::Twine("'") + name + "' of " + file).str();
}

std::string ErrorTextLateUse::sameFile(llvm::StringRef callee, llvm::StringRef var) {
  return (llvm::Twine(callee) + " uses '" + var +
          "' before it is declared: JavaScript would throw a ReferenceError when it runs here; "
          "move the declaration of '" +
          var + "' up")
      .str();
}

std::string ErrorTextLateUse::otherFile(llvm::StringRef file, llvm::StringRef callee,
                                        llvm::StringRef var, llvm::StringRef home,
                                        llvm::StringRef chain) {
  return (llvm::Twine(file) + " calls " + callee + " while it loads, and it reads '" + var +
          "' of " + home + ", which is not loaded yet: " + chain +
          "; make the call inside a function, or move '" + var + "' to a file outside the cycle")
      .str();
}

std::string ErrorTextLateUse::usedHere(llvm::StringRef var) {
  return (llvm::Twine("'") + var + "' is used here").str();
}

std::string ErrorTextLateUse::declared(llvm::StringRef var) {
  return (llvm::Twine("'") + var + "' is declared here").str();
}

std::string ErrorTextLateUse::function(llvm::StringRef name) {
  return ErrorTextFunctionName::quoted(name);
}

std::string ErrorTextLateUse::creation(llvm::StringRef className) {
  return (llvm::Twine("'new ") + className + "()'").str();
}

std::string ErrorTextLateUse::method(llvm::StringRef className, llvm::StringRef name) {
  return (llvm::Twine("'") + className + "." + name + "'").str();
}

}
