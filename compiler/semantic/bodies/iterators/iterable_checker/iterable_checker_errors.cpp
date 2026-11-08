#include "compiler/semantic/bodies/iterators/iterable_checker/iterable_checker_errors.h"
#include "llvm/ADT/Twine.h"

namespace tz {

std::string ErrorTextIteratorGiven::notIterator(llvm::StringRef type, llvm::StringRef gives) {
  return (llvm::Twine("'[Symbol.iterator]()' of '") + type + "' gives '" + gives +
          "', which is not an iterator: it needs a method 'next()' that gives "
          "'{ done, value }' (IteratorResult<T>); or make it a generator, "
          "'*[Symbol.iterator]() { yield ... }'")
      .str();
}

std::string ErrorTextAsyncGiven::notAsync(llvm::StringRef type, llvm::StringRef gives) {
  return (llvm::Twine("'[Symbol.asyncIterator]()' of '") + type + "' gives '" + gives +
          "', which is not an async iterator; make it an async generator, "
          "'async *[Symbol.asyncIterator]() { yield ... }'")
      .str();
}

std::string ErrorTextIterableParam::typeArgs() {
  return "'Iterable' takes one type argument, the type of its values, e.g. 'Iterable<int>'";
}

std::string ErrorTextIterableParam::inCtor(llvm::StringRef element) {
  return (llvm::Twine("a constructor cannot take an 'Iterable<") + element +
          ">' yet: it would make a copy of the constructor for each kind of "
          "iterable; take an array, e.g. '" +
          element +
          "[]', and pass "
          "'[...xs]'")
      .str();
}

std::string ErrorTextIterableParam::notTop(llvm::StringRef element) {
  return (llvm::Twine("a parameter 'Iterable<") + element +
          ">' makes the function generic, and a generic function can be "
          "declared only at the top level for now; take an array, e.g. '" +
          element + "[]'")
      .str();
}

}
