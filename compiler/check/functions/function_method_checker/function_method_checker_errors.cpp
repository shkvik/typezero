#include "compiler/check/functions/function_method_checker/function_method_checker_errors.h"
#include "llvm/ADT/Twine.h"

namespace tz {

std::string ErrorTextThisCall::noBind() {
  return "'bind' is not supported: pass the 'this' with every call, "
         "'f.call(obj, ...)'";
}

std::string ErrorTextThisCall::noProperty(llvm::StringRef name) {
  return (llvm::Twine("a function has no property '") + name +
          "'; call it with '.call(obj, ...)' or '.apply(obj, args)'")
      .str();
}

std::string ErrorTextThisCall::thisFirst(llvm::StringRef name) {
  return (llvm::Twine("'") + name + "' takes the 'this' of the call first, e.g. 'f." + name +
          "(this" + (name == "apply" ? ", args)'" : ", ...)'"))
      .str();
}

std::string ErrorTextThisCall::applyArgs() {
  return "'apply' takes two arguments: the 'this' of the call and the list "
         "of the arguments, e.g. 'f.apply(this, args)'";
}

std::string ErrorTextListMethod::noMethod(llvm::StringRef name) {
  return (llvm::Twine("a list of parameters has no method '") + name +
          "' here; it has 'length' and 'join', and goes on with "
          "'...args'")
      .str();
}

}
