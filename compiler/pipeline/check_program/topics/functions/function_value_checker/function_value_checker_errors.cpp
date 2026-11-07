#include "compiler/pipeline/check_program/topics/functions/function_value_checker/function_value_checker_errors.h"
#include "llvm/ADT/Twine.h"

namespace tz {

std::string ErrorTextExtraArguments::passed(llvm::StringRef function, llvm::StringRef argument,
                                            llvm::StringRef ordinal, llvm::StringRef method,
                                            llvm::StringRef passes) {
  return (llvm::Twine("'") + function + "' would also get the " + argument + " as its " + ordinal +
          " argument: '" + method + "' calls it with " + passes +
          "; pass only what it needs: 'x => " + function + "(x)'")
      .str();
}

std::string ErrorTextAsyncCallback::replace(llvm::StringRef method, llvm::StringRef type) {
  return (llvm::Twine("'") + method + "' uses what the function returns as '" + type +
          "', but an async function returns a Promise; JavaScript would insert \"[object "
          "Promise]\" "
          "for each match: work out the replacements first, then replace with a function that is "
          "not async")
      .str();
}

std::string ErrorTextAsyncCallback::test(llvm::StringRef method, llvm::StringRef type) {
  return (llvm::Twine("'") + method + "' uses what the function returns as '" + type +
          "', but an async function returns a Promise; a Promise is always truthy, so JavaScript "
          "would take every element as passing: await the tests first, e.g. 'const passed = await "
          "Promise.all(xs.map(async x => ...))', then test 'passed[i]'")
      .str();
}

std::string ErrorTextAsyncCallback::value(llvm::StringRef method, llvm::StringRef type) {
  return (llvm::Twine("'") + method + "' uses what the function returns as '" + type +
          "', but an async function returns a Promise; drop 'async', or await what the function "
          "needs before the call")
      .str();
}

}
