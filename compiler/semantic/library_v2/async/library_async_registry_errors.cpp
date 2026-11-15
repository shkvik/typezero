#include "compiler/semantic/library_v2/async/library_async_registry_errors.h"
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

std::string ErrorTextPromiseValue::typeArgs() {
  return "'Promise' takes one type argument, the type of its value";
}

std::string ErrorTextPromiseValue::notInferred() {
  return "cannot tell the type of the Promise's value from what is given "
         "to 'resolve'; write it, e.g. 'new Promise<int>(...)'";
}

std::string ErrorTextExecutor::rejects() {
  return "a Promise is not rejected in tz, so its function gets only "
         "'resolve'; an operation that may fail gives a Promise<Result<T, E>>: "
         "'resolve(err(e))'";
}

std::string ErrorTextPromiseMethod::onResult(llvm::StringRef name, llvm::StringRef promise) {
  return (llvm::Twine("'") + name +
          "' is a method of Result, not of Promise; await the Promise first: "
          "'(await " +
          promise + ")." + name + "(...)'")
      .str();
}

std::string ErrorTextPromiseMethod::noCatch() {
  return "a Promise is not rejected in tz, so it has no 'catch': an operation "
         "that may fail gives a Promise<Result<T, E>>; read it after 'await': "
         "'const r = await p; if (r.isErr()) ...'";
}

std::string ErrorTextPromiseMethod::noFinally() {
  return "a Promise is not rejected in tz, so it has no 'finally': 'then' runs "
         "every time; write the code after 'await p'";
}

std::string ErrorTextPromiseMethod::thenTwo() {
  return "'then' takes one function: a Promise is not rejected in tz";
}

std::string ErrorTextPromiseMethod::noMethod(llvm::StringRef name, llvm::StringRef type) {
  return (llvm::Twine("property '") + name + "' does not exist on type '" + type +
          "'; a Promise has 'then', and its value is "
          "read with 'await'")
      .str();
}

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

std::string ErrorTextPromiseList::valuesLabel() {
  return "values";
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

std::string ErrorTextTaskCallback::notFunction(llvm::StringRef fn, llvm::StringRef type) {
  return (llvm::Twine("'") + fn + "' needs a function, found '" + type + "'").str();
}

std::string ErrorTextTaskCallback::nothing(llvm::StringRef fn, llvm::StringRef type) {
  return (llvm::Twine("'") + fn + "' calls its callback with nothing, but this function is '" +
          type + "'; pass an arrow that calls it, e.g. '() => f()'")
      .str();
}

std::string ErrorTextAsyncGlobal::callbackArg() {
  return "arguments for the callback are not supported; pass an "
         "arrow that calls it with them";
}

std::string ErrorTextTimerMethod::noMethod(llvm::StringRef name, llvm::StringRef type,
                                           bool immediate) {
  return (llvm::Twine("property '") + name + "' does not exist on type '" + type +
          "'; it has 'ref', 'unref', 'hasRef'" + (immediate ? "" : " and 'refresh'"))
      .str();
}

std::string ErrorTextTimersPromises::scheduler() {
  return "'scheduler' is used through its functions: 'scheduler.wait(ms)' or "
         "'scheduler.yield()'";
}

std::string ErrorTextTimersPromises::voidValue(llvm::StringRef fn) {
  return (llvm::Twine("a value of type 'void' cannot be given to '") + fn + "'").str();
}

std::string ErrorTextScheduler::noFunction(llvm::StringRef name) {
  return (llvm::Twine("'scheduler.") + name + "' does not exist; scheduler has 'wait' and 'yield'")
      .str();
}

}
