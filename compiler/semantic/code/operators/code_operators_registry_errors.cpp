#include "compiler/semantic/code/operators/code_operators_registry_errors.h"
#include "llvm/ADT/Twine.h"

namespace tz {

std::string ErrorTextOperands::types(llvm::StringRef op, llvm::StringRef lhs, llvm::StringRef rhs,
                                     bool undefined) {
  std::string message = (llvm::Twine("operator '") + op + "' cannot be applied to types '" + lhs +
                         "' and '" + rhs + "'")
                            .str();
  if (undefined) {
    message += "; a value may be undefined: check it first or give a default with '??'";
  }

  return message;
}

std::string ErrorTextUnary::noNumber(llvm::StringRef op, bool string) {
  return (llvm::Twine("unary '") + op + "' does not turn a " + (string ? "string" : "boolean") +
          " into a number in tz; write " +
          (string ? "'Number(s)', which gives a Result" : "'Number(b)'"))
      .str();
}

std::string ErrorTextUnary::decimalSum() {
  return "unary '+' cannot be applied to a decimal; for a double, write "
         "'Number(x)', or drop the '+'";
}

std::string ErrorTextUnary::notNumber(llvm::StringRef op, llvm::StringRef type) {
  return (llvm::Twine("operator '") + op + "' cannot be applied to type '" + type + "'").str();
}

std::string ErrorTextUnary::bitsDouble() {
  return "operator '~' works on the bits of integers, and this "
         "is a double; drop the fraction first, e.g. "
         "'Math.trunc(x).toInt()'";
}

std::string ErrorTextUnary::bitsType(llvm::StringRef type) {
  return (llvm::Twine("operator '~' cannot be applied to type '") + type + "'").str();
}

std::string ErrorTextUnary::dropsAsync() {
  return "'void' drops the Result that the Promise gives, so an error in "
         "it would go unchecked; let the task handle it, e.g. an async "
         "function that checks 'if (r.isErr()) ...'";
}

std::string ErrorTextUnary::drops() {
  return "'void' drops the Result, so an error in it would go unchecked; "
         "handle it, e.g. with '.match(...)'";
}

std::string ErrorTextUnary::allUndef() {
  return "'!' is not supported, and the value is always undefined";
}

std::string ErrorTextUnary::neverUndef(llvm::StringRef text) {
  return (llvm::Twine("'!' is not supported, and '") + text +
          "' is never undefined here; drop the '!'")
      .str();
}

std::string ErrorTextUnary::checkFirst(llvm::StringRef text) {
  return (llvm::Twine("'!' is not supported: check the value first, e.g. 'if (") + text +
          " == undefined) return ...', or give a "
          "default with '" +
          text + " ?? ...'")
      .str();
}

}
