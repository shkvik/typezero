#include "compiler/semantic/bodies/expr/expr_checker/expr_checker_errors.h"
#include "llvm/ADT/Twine.h"

namespace tz {

std::string ErrorTextConditional::mismatch(llvm::StringRef thenType, llvm::StringRef elseType) {
  return (llvm::Twine("the branches of '?:' have incompatible types '") + thenType + "' and '" +
          elseType + "'")
      .str();
}

std::string ErrorTextCast::plain() {
  return "'as' is not supported";
}

std::string ErrorTextCast::declare(llvm::StringRef to) {
  return (llvm::Twine(
              "'as' is not supported; declare the type of the variable instead, e.g. 'let x: ") +
          to + " = ...'")
      .str();
}

std::string ErrorTextCast::itself(llvm::StringRef to) {
  return (llvm::Twine("'as' is not supported; the value converts to '") + to +
          "' by itself, so remove 'as'")
      .str();
}

std::string ErrorTextCast::undefined() {
  return "'as' is not supported; the value may be undefined: check it first (e.g. 'x != "
         "undefined') or give a default with '??'";
}

std::string ErrorTextCast::hinted(llvm::StringRef hint) {
  return (llvm::Twine("'as' is not supported") + hint).str();
}

std::string ErrorTextCast::toDecimal() {
  return "'as' is not supported; convert the double with 'decimal(x)'";
}

std::string ErrorTextCast::toNumber() {
  return "'as' is not supported; 'Number(x)' gives the nearest double";
}

std::string ErrorTextCast::json() {
  return "'as' is not supported; check what a Json holds first, e.g. 'typeof x == \"number\"', and "
         "the check narrows it";
}

std::string ErrorTextCast::cannot(llvm::StringRef from, llvm::StringRef to) {
  return (llvm::Twine("'as' is not supported, and type '") + from + "' cannot become '" + to + "'")
      .str();
}

}
