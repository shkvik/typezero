#include "compiler/semantic/bodies/functions/callback_checker/callback_checker_errors.h"
#include "compiler/program/library/array/array_method_table/array_method_table.h"
#include "llvm/ADT/Twine.h"

namespace tz {

std::string ErrorTextCallbackArg::notFunc(llvm::StringRef method, llvm::StringRef type) {
  return (llvm::Twine("'") + method + "' needs a function, found '" + type + "'").str();
}

std::string ErrorTextCallbackArg::rest(llvm::StringRef name) {
  return (llvm::Twine(name) + " takes a rest parameter; pass an arrow that calls it instead").str();
}

std::string ErrorTextCallbackArg::paramType(llvm::StringRef name, llvm::StringRef param,
                                            llvm::StringRef ordinal, llvm::StringRef method,
                                            llvm::StringRef given) {
  return (llvm::Twine(name) + " takes '" + param + "' as its " + ordinal + " argument, but '" +
          method + "' passes '" + given + "'")
      .str();
}

std::string ErrorTextCallbackArg::extraParam(llvm::StringRef name, llvm::StringRef argName,
                                             llvm::StringRef paramName, llvm::StringRef method,
                                             llvm::StringRef args) {
  return (llvm::Twine(name) + " would also get the " + argName + " in its optional parameter" +
          paramName + ": '" + method + "' calls it with " + args +
          "; pass an arrow that gives it only what it needs")
      .str();
}

std::string ErrorTextCallbackArg::tooFew(llvm::StringRef method, llvm::StringRef passes,
                                         llvm::StringRef name, llvm::StringRef needs) {
  return (llvm::Twine("'") + method + "' passes " + passes + ", but " + name + " needs " + needs)
      .str();
}

std::string ErrorTextCallbackArg::badReturn(llvm::StringRef method, llvm::StringRef wanted,
                                            llvm::StringRef name, llvm::StringRef returns) {
  return (llvm::Twine("'") + method + "' needs a function that returns '" + wanted + "', but " +
          name + " returns '" + returns + "'")
      .str();
}

std::string ErrorTextCallbackArg::noCheck(llvm::StringRef name, llvm::StringRef method) {
  return (llvm::Twine(name) + " returns a Result that '" + method +
          "' drops, so an error in it would go unchecked; pass an arrow "
          "that handles it, e.g. with '.match(...)'")
      .str();
}

std::string ErrorTextCallbackArg::noWait(llvm::StringRef name, llvm::StringRef method) {
  return (llvm::Twine(name) + " returns a Promise that '" + method +
          "' drops, so nothing waits for it to finish; loop with 'for "
          "(const x of xs) await f(x)', or wait for all with 'await "
          "Promise.all(xs.map(...))'")
      .str();
}

std::string ErrorTextCallbackMethod::noFunction(llvm::StringRef name) {
  return (llvm::Twine("'") + name + "' needs a function").str();
}

std::string ErrorTextCallbackMethod::thisArg(llvm::StringRef name) {
  return (llvm::Twine("the second argument of '") + name +
          "' sets 'this' for the callback, which is not supported")
      .str();
}

std::string ErrorTextCallbackMethod::untyped(llvm::StringRef name) {
  return (llvm::Twine("the initial value of '") + name +
          "' needs a type; declare it in a "
          "variable first, e.g. 'const start: int | undefined = undefined'")
      .str();
}

std::string ErrorTextCallbackMethod::noValue(llvm::StringRef name) {
  return (llvm::Twine("the callback of '") + name + "' must return a value").str();
}

std::string ErrorTextCallbackMethod::notNumber(llvm::StringRef type) {
  return (llvm::Twine("a comparison function must return a number (negative, zero "
                      "or positive), found '") +
          type + "'")
      .str();
}

std::string ErrorTextCallbackArg::argsShape(llvm::StringRef method) {
  std::optional<ArrayCallback> callback = ArrayMethodTable::findCallback(method);
  if (callback == ArrayCallback::REDUCE) {
    return "(accumulator, value, index, array)";
  }
  if (callback == ArrayCallback::COMPARE) {
    return "(a, b)";
  }

  return "(value, index, array)";
}

std::string ErrorTextCallbackArg::position(size_t index) {
  static const char *const names[] = {"first", "second", "third", "fourth", "fifth"};

  return index < 5 ? names[index] : "next";
}

std::string ErrorTextCallbackArg::argName(llvm::StringRef method, size_t index) {
  std::optional<ArrayCallback> callback = ArrayMethodTable::findCallback(method);
  if (callback == ArrayCallback::REDUCE) {
    return index == 0 ? "accumulator" : index == 1 ? "value" : index == 2 ? "index" : "array";
  }

  return index == 0 ? "value" : index == 1 ? "index" : "array";
}

std::string ErrorTextCallbackArg::paramName(llvm::StringRef name) {
  return (llvm::Twine(" '") + name + "'").str();
}

}
