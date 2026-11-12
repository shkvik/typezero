#include "compiler/semantic/bodies/iterators/generator_checker/generator_checker_errors.h"
#include "llvm/ADT/Twine.h"

namespace tz {

std::string ErrorTextIteratorType::resultArgs() {
  return "'IteratorResult' takes 1-2 type arguments: the values and what the "
         "iterator gives when it is done, e.g. 'IteratorResult<int, undefined>'";
}

std::string ErrorTextIteratorType::iterable() {
  return "'Iterable<T>' can only be the type of a parameter of a function declared "
         "at the top level or of a method, which it makes generic: a copy for "
         "each kind of iterable; here, write the type itself, e.g. 'int[]', or "
         "'Iterator<int>' for any iterator";
}

std::string ErrorTextIteratorType::typeArgs(llvm::StringRef name) {
  return (llvm::Twine("'") + name +
          "' takes 1-3 type arguments: the values it gives, what it "
          "returns and what next() sends, e.g. '" +
          name + "<int>'")
      .str();
}

std::string ErrorTextGeneratorType::async(llvm::StringRef yielded) {
  return (llvm::Twine("an async generator returns an async iterator: declare its type as "
                      "'AsyncGenerator<") +
          yielded + ">'")
      .str();
}

std::string ErrorTextGeneratorType::sync(llvm::StringRef declared) {
  return (llvm::Twine("a generator returns an iterator: declare its type as 'Generator<") +
          declared + ">'")
      .str();
}

std::string ErrorTextSendsNothing::destructuring() {
  return "destructuring";
}

std::string ErrorTextSendsNothing::expects(llvm::StringRef what, llvm::StringRef iterator,
                                           llvm::StringRef sent) {
  return (llvm::Twine(what) + " resumes each 'yield' of '" + iterator +
          "' without a value, but it expects '" + sent +
          "' from next(x); call next(x) yourself, or let the generator take '" + sent +
          " | undefined'")
      .str();
}

std::string ErrorTextYields::differ(llvm::StringRef fn, llvm::StringRef here,
                                    llvm::StringRef elsewhere) {
  return (llvm::Twine(fn) + " yields '" + here + "' here and '" + elsewhere +
          "' elsewhere; a generator yields values of "
          "one type")
      .str();
}

std::string ErrorTextYields::unknown(llvm::StringRef fn, llvm::StringRef why) {
  return (llvm::Twine("cannot tell what ") + fn + " yields" + why +
          "; declare its type, e.g. ': Generator<int>'")
      .str();
}

std::string ErrorTextYields::noYield() {
  return ": it has no 'yield'";
}

std::string ErrorTextYields::onlyUndef() {
  return ": it yields only undefined";
}

std::string ErrorTextYield::asyncSent(llvm::StringRef yielded) {
  return (llvm::Twine("values sent with next(x) to an async generator are not "
                      "supported yet; declare it as 'AsyncGenerator<") +
          yielded + ">'")
      .str();
}

std::string ErrorTextYield::mixed(llvm::StringRef type) {
  return (llvm::Twine("'yield' of '") + type +
          "' gives the value of the Promise or the value "
          "itself, which make no one type; check what it "
          "holds first")
      .str();
}

std::string ErrorTextYield::promise(llvm::StringRef given, llvm::StringRef fn,
                                    llvm::StringRef yielded) {
  return (llvm::Twine("the Promise gives '") + given + "', but " + fn + " yields '" + yielded + "'")
      .str();
}

std::string ErrorTextYield::noValue(llvm::StringRef fn, llvm::StringRef yielded) {
  return (llvm::Twine("'yield' without a value gives undefined, but ") + fn + " yields '" +
          yielded + "'")
      .str();
}

std::string ErrorTextYield::notAsync(llvm::StringRef type) {
  return (llvm::Twine("'yield*' in an async generator needs an async "
                      "iterator, an array or an iterator, found '") +
          type + "'")
      .str();
}

std::string ErrorTextYield::otherType(llvm::StringRef element, llvm::StringRef fn,
                                      llvm::StringRef yielded) {
  return (llvm::Twine("'yield*' gives values of type '") + element + "', but " + fn + " yields '" +
          yielded + "'")
      .str();
}

std::string ErrorTextYield::notIter(llvm::StringRef type) {
  return (llvm::Twine("'yield*' needs an array, a string or an iterator, found '") + type + "'")
      .str();
}

std::string ErrorTextYield::sends(llvm::StringRef type, llvm::StringRef expects, llvm::StringRef fn,
                                  llvm::StringRef gets) {
  return (llvm::Twine("'yield*' passes on what next(x) sends, and '") + type + "' expects '" +
          expects + "' from it, but " + fn + " gets " + gets)
      .str();
}

std::string ErrorTextYield::untyped(llvm::StringRef yielded) {
  return (llvm::Twine("the value of 'yield' is what next(x) sends, and nothing says its type; "
                      "declare it in the return type, e.g. ': Generator<") +
          yielded + ", void, string>'")
      .str();
}

std::string ErrorTextYield::nothing() {
  return "nothing";
}

}
