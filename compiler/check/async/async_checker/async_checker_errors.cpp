#include "compiler/check/async/async_checker/async_checker_errors.h"
#include "llvm/ADT/Twine.h"

namespace tz {

std::string ErrorTextPromiseType::typeArgs(llvm::StringRef name) {
  return (llvm::Twine("'") + name + "' takes one type argument, the type of its value, e.g. '" +
          name + "<int>'")
      .str();
}

std::string ErrorTextPromiseType::notPromise(llvm::StringRef type) {
  return (llvm::Twine("an async function returns a Promise: declare its type as "
                      "'Promise<") +
          type + ">'")
      .str();
}

std::string ErrorTextAsyncReturns::differ(llvm::StringRef fn, llvm::StringRef here,
                                          llvm::StringRef elsewhere) {
  return (llvm::Twine(fn) + " returns '" + here + "' here and '" + elsewhere +
          "' elsewhere; a function returns values of one type")
      .str();
}

std::string ErrorTextAsyncReturns::promise(llvm::StringRef given, llvm::StringRef fn,
                                           llvm::StringRef value) {
  return (llvm::Twine("this Promise gives '") + given + "', but " + fn + " gives '" + value +
          "'; await it and convert the value")
      .str();
}

std::string ErrorTextAsyncContext::await(llvm::StringRef hint) {
  return (llvm::Twine("'await' can only be used in an async function or in top-level "
                      "code; make the function async: ") +
          hint)
      .str();
}

std::string ErrorTextAsyncContext::forAwait() {
  return "'for await' can only be used in an async function or in top-level "
         "code; make the function async";
}

std::string ErrorTextAwaited::mixed(llvm::StringRef type) {
  return (llvm::Twine("'await' of '") + type +
          "' gives the value of the Promise or the value itself, which make no one "
          "type; check what it holds first")
      .str();
}

std::string ErrorTextAwaitedElement::undefined(llvm::StringRef type) {
  return (llvm::Twine("'") + type + "' may be undefined; check it before iterating").str();
}

std::string ErrorTextAwaitedElement::record() {
  return "a Record is not iterable, as an object is not in JavaScript; walk its keys "
         "with 'for...in', or its entries with 'for (const [k, v] of "
         "Object.entries(r))'";
}

std::string ErrorTextAwaitedElement::notAsync(llvm::StringRef type) {
  return (llvm::Twine("'for await' needs an async iterator, an array or an "
                      "iterator, found '") +
          type + "'")
      .str();
}

std::string ErrorTextBindingType::annotated() {
  return "a 'for await' variable cannot have a type annotation";
}

std::string ErrorTextBindingShape::notArray(llvm::StringRef type) {
  return (llvm::Twine("only arrays can be destructured; the elements are '") + type + "'").str();
}

std::string ErrorTextDiscarded::mapped() {
  return "the Promises are not awaited, so nothing waits for them and an "
         "error in one would end the program; wait for all of them with "
         "'await Promise.all(...)', or keep them in a variable";
}

std::string ErrorTextDiscarded::callback() {
  return "the function returns a Promise that its caller drops, so nothing "
         "waits for it to finish; loop with 'for (const x of xs) await "
         "f(x)', or wait for all with 'await Promise.all(xs.map(...))'";
}

std::string ErrorTextDiscarded::result(llvm::StringRef text) {
  return (llvm::Twine("the Promise is not awaited, and the Result it gives would go "
                      "unchecked; await it and handle the Result, e.g. 'const r = await ") +
          text + "'")
      .str();
}

std::string ErrorTextDiscarded::plain() {
  return "the Promise is not awaited, so nothing waits for it and an error in "
         "it would end the program later; write 'await' before it, or "
         "'void' to let it run on its own";
}

std::string ErrorTextDiscarded::withText(llvm::StringRef text) {
  return (llvm::Twine("the Promise is not awaited, so nothing waits for it and an error in "
                      "it would end the program later; write 'await ") +
          text + "', or 'void " + text + "' to let it run on its own")
      .str();
}

std::string ErrorTextAsyncContext::arrowHint() {
  return "'async (...) => ...'";
}

std::string ErrorTextAsyncContext::methodHint(llvm::StringRef name) {
  return (llvm::Twine("'async ") + name + "() { ... }'").str();
}

std::string ErrorTextAsyncContext::namedHint(llvm::StringRef name) {
  return (llvm::Twine("'async function ") + name + "'").str();
}

std::string ErrorTextAsyncContext::plainHint() {
  return "'async function'";
}

std::string ErrorTextAsyncContext::awaitLoop() {
  return "'for await'";
}

}
