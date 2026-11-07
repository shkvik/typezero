#include "compiler/pipeline/check_program/topics/async/promise_checker/promise_checker_errors.h"
#include "llvm/ADT/Twine.h"

namespace tz {

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

}
