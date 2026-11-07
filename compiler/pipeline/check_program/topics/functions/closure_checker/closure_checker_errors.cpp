#include "compiler/pipeline/check_program/topics/functions/closure_checker/closure_checker_errors.h"
#include "llvm/ADT/Twine.h"

namespace tz {

std::string ErrorTextNotGeneric::generic() {
  return "a generic arrow or function expression can only be the value of a top-level "
         "constant for now, e.g. 'const f = <T>(x: T) => x'; or declare a generic function "
         "at the top level: 'function f<T>(x: T) { ... }'";
}

std::string ErrorTextParamAgrees::declared(llvm::StringRef label, llvm::StringRef passed,
                                           llvm::StringRef given) {
  return (llvm::Twine("parameter '") + label + "' is declared as '" + passed +
          "', but the function gets '" + given + "' here")
      .str();
}

std::string ErrorTextDefaultMatters::unused(llvm::StringRef label) {
  return (llvm::Twine("the default value of '") + label +
          "' is never used: the function always gets a value here")
      .str();
}

std::string ErrorTextParamTypeKnown::needsType(llvm::StringRef label) {
  return (llvm::Twine("parameter '") + label +
          "' needs a type: nothing here says what the function is called with")
      .str();
}

std::string ErrorTextThisParam::arrowThis() {
  return "an arrow function cannot have a 'this' parameter: it sees the "
         "'this' of the function around it; write a 'function'";
}

std::string ErrorTextRestParam::notArray(llvm::StringRef name) {
  return (llvm::Twine("a rest parameter must be an array, e.g. '...") + name + ": int[]'").str();
}

std::string ErrorTextParamOrder::order() {
  return "a required parameter cannot follow an optional one";
}

std::string ErrorTextPromiseAwaited::dropped() {
  return "the Promise of this async function is dropped here, so nothing "
         "waits for it to finish; loop with 'for (const x of xs) await "
         "f(x)', or wait for all with 'await Promise.all(xs.map(async x "
         "=> ...))'";
}

}
