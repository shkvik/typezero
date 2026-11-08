#include "compiler/semantic/bodies/values/number_methods/number_methods_errors.h"
#include "llvm/ADT/Twine.h"

namespace tz {

std::string ErrorTextNumberStatic::rawTag() {
  return "String.raw can only tag a template, e.g. String.raw`C:\\dir`";
}

std::string ErrorTextNumberStatic::noProperty(llvm::StringRef name, llvm::StringRef owner) {
  return (llvm::Twine("property '") + name + "' does not exist on '" + owner + "'").str();
}

std::string ErrorTextConversion::toDecimal(llvm::StringRef value) {
  return (llvm::Twine("a double is inexact already, so it becomes a decimal through "
                      "'decimal(") +
          value + ")', which says so")
      .str();
}

std::string ErrorTextConversion::toInteger(llvm::StringRef value, llvm::StringRef method) {
  return (llvm::Twine("a double becomes an integer only after rounding, which says what "
                      "happens to the fraction: 'Math.trunc(") +
          value + ")." + method + "()' (or Math.round, Math.floor, Math.ceil)")
      .str();
}

std::string ErrorTextNumberMethod::noProperty(llvm::StringRef name, llvm::StringRef type) {
  return (llvm::Twine("property '") + name + "' does not exist on type '" + type + "'").str();
}

std::string ErrorTextNumberMethod::codePoint() {
  return "a code point";
}

std::string ErrorTextNumberMethod::radix() {
  return "the radix of 'toString'";
}

std::string ErrorTextNumberMethod::digits() {
  return "the number of digits of 'toFixed'";
}

}
