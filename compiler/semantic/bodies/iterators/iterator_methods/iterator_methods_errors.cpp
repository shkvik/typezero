#include "compiler/semantic/bodies/iterators/iterator_methods/iterator_methods_errors.h"
#include "llvm/ADT/Twine.h"

namespace tz {

std::string ErrorTextStaticFrom::iterator(llvm::StringRef name) {
  return (llvm::Twine("'Iterator.") + name +
          "' is not supported; of Iterator only 'Iterator.from' is")
      .str();
}

std::string ErrorTextStaticFrom::array(llvm::StringRef name) {
  return (llvm::Twine("'Array.") + name +
          "' is not supported; of Array only 'Array.from' and 'new Array' are")
      .str();
}

std::string ErrorTextIterSource::notIter(llvm::StringRef fn, llvm::StringRef type) {
  return (llvm::Twine("'") + fn + "' needs an array, a string or an iterator, found '" + type + "'")
      .str();
}

std::string ErrorTextCallbackValue::missing(llvm::StringRef fn) {
  return (llvm::Twine("the callback of '") + fn + "' must return a value").str();
}

std::string ErrorTextNextValue::noUnion(llvm::StringRef iterator, llvm::StringRef element,
                                        llvm::StringRef returned, llvm::StringRef problem) {
  return (llvm::Twine("next() of '") + iterator + "' gives '" + element + "', or '" + returned +
          "' when it is done, and no union holds both: " + problem +
          "; iterate it with 'for...of', which ignores what it returns")
      .str();
}

std::string ErrorTextIteratorMethod::noThrow() {
  return "an iterator has no 'throw()' in tz: an error is a value, "
         "not something thrown into a generator";
}

std::string ErrorTextIteratorMethod::noReturn() {
  return "'return()' of an iterator is not supported yet; stop "
         "with 'break' in 'for of'";
}

std::string ErrorTextIteratorMethod::noProperty(llvm::StringRef name, llvm::StringRef type) {
  std::string message =
      (llvm::Twine("property '") + name + "' does not exist on type '" + type + "'").str();
  if (name == "length") {
    message += "; an iterator does not know how many values it gives: collect them with '[...it]'";
  }

  return message;
}

std::string ErrorTextIteratorMethod::noFunction(llvm::StringRef name) {
  return (llvm::Twine("'") + name + "' needs a function").str();
}

std::string ErrorTextReduceInit::untyped() {
  return "the initial value of 'reduce' needs a type; declare it in a variable "
         "first, e.g. 'const start: int | undefined = undefined'";
}

std::string ErrorTextFlatMap::notIter(llvm::StringRef type) {
  return (llvm::Twine("the callback of 'flatMap' must return an array or an iterator, "
                      "found '") +
          type + "'")
      .str();
}

std::string ErrorTextIteratorMethod::countOf(llvm::StringRef name) {
  return (llvm::Twine("the count of '") + name + "'").str();
}

}
