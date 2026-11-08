#include "compiler/semantic/library/async/promise_static_checker/promise_static_checker_errors.h"
#include "llvm/ADT/Twine.h"

namespace tz {

std::string ErrorTextStaticTypeArgs::resolvers() {
  return "type arguments can be given to 'Promise.withResolvers<T>()'; "
         "the other functions of Promise take their types from what they "
         "get";
}

std::string ErrorTextPromiseVoid::given(llvm::StringRef fn) {
  return (llvm::Twine("a value of type 'void' cannot be given to '") + fn + "'").str();
}

std::string ErrorTextResolverValue::typeArgs() {
  return "'Promise.withResolvers' takes one type argument, the type of "
         "the value";
}

std::string ErrorTextResolverValue::unknown() {
  return "'Promise.withResolvers' needs the type of the value; write it, "
         "e.g. 'Promise.withResolvers<int>()'";
}

std::string ErrorTextRaceJoin::differ(llvm::StringRef fn, llvm::StringRef type,
                                      llvm::StringRef other) {
  return (llvm::Twine("'") + fn +
          "' gives a value of one of the promises, "
          "but they give '" +
          type + "' and '" + other + "'; make them give one type")
      .str();
}

std::string ErrorTextAllValues::voidValue() {
  return "'Promise.all' collects the values, and a "
         "Promise<void> has none; await it on its own";
}

std::string ErrorTextPromiseList::noType(llvm::StringRef fn) {
  return (llvm::Twine("'") + fn +
          "' of no promises needs the type of their values; "
          "give the array a type, e.g. 'const ps: Promise<int>[] = []'")
      .str();
}

std::string ErrorTextPromiseList::mayBe(llvm::StringRef fn, llvm::StringRef type) {
  return (llvm::Twine("'") + fn + "' of values that may be promises ('" + type +
          "') is not supported yet; await each first, or make each a Promise")
      .str();
}

std::string ErrorTextPromiseList::notArray(llvm::StringRef fn, llvm::StringRef type, bool iterable,
                                           llvm::StringRef values) {
  std::string spread =
      iterable ? (llvm::Twine("; make an array of them: '") + fn + "([..." + values + "])'").str()
               : "";

  return (llvm::Twine("'") + fn + "' needs an array of promises, found '" + type + "'" + spread)
      .str();
}

std::string ErrorTextNoReject::rejected() {
  return "a Promise is not rejected in tz: an operation that may fail gives a "
         "Promise<Result<T, E>>; give the error in the value, "
         "'Promise.resolve(err(e))'";
}

std::string ErrorTextNoReject::settled() {
  return "'Promise.allSettled' is not needed: a Promise is not rejected, so "
         "'Promise.all' gives every value; with Results inside, "
         "'Result.combine(await Promise.all(ps))' gives all the values or the "
         "first Err";
}

std::string ErrorTextNoReject::any() {
  return "'Promise.any' is not needed: a Promise is not rejected, so the first "
         "to settle gives the first value, 'Promise.race(ps)'";
}

std::string ErrorTextPromiseStatic::unknown(llvm::StringRef name) {
  return (llvm::Twine("'Promise.") + name +
          "' is not supported; the functions of Promise "
          "are 'resolve', 'all', 'race' and 'withResolvers'")
      .str();
}

}
