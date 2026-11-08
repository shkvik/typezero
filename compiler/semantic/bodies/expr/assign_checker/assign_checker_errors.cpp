#include "compiler/semantic/bodies/expr/assign_checker/assign_checker_errors.h"
#include "llvm/ADT/Twine.h"

namespace tz {

std::string ErrorTextAssign::imported(llvm::StringRef name) {
  return (llvm::Twine("cannot assign to '") + name + "' because it is imported").str();
}

std::string ErrorTextAssign::nullishVar() {
  return "only variables can be the target of '??='";
}

std::string ErrorTextAssign::notOptional(llvm::StringRef name, llvm::StringRef type) {
  return (llvm::Twine("'??=' needs a variable that may be undefined; '") + name + "' has type '" +
          type + "'")
      .str();
}

std::string ErrorTextAssign::indexRange() {
  return "cannot prove that the index is within the array; check it first, "
         "e.g. 'if (i < a.length)'";
}

std::string ErrorTextAssign::untyped(llvm::StringRef name) {
  return (llvm::Twine("cannot infer the type of field '") + name +
          "' from this value; declare it, e.g. '" + name + ": int | undefined = undefined'")
      .str();
}

std::string ErrorTextAssign::outOfRange() {
  return "cannot prove that the index is within the array (0 <= index <= length); writing further "
         "would leave holes. Check the index first, e.g. 'if (i < a.length)', or use push()";
}

std::string ErrorTextAssign::lengthRange(llvm::StringRef element, llvm::StringRef array) {
  return (llvm::Twine("cannot prove that the new length is from 0 to the old one: an array "
                      "of '") +
          element +
          "' cannot grow by setting its length, which would leave elements "
          "without a value; check it first, e.g. 'if (n >= 0 && n <= " +
          array + ".length)', or use push()")
      .str();
}

std::string ErrorTextCompound::joinsText(llvm::StringRef value, llvm::StringRef hint) {
  return (llvm::Twine("operator '+=' cannot be applied to types 'string' and '") + value +
          "' (JavaScript would join them as text)" + hint)
      .str();
}

std::string ErrorTextCompound::convertTo() {
  return "; convert the value with String()";
}

std::string ErrorTextCompound::writeAs(llvm::StringRef target, llvm::StringRef value) {
  return (llvm::Twine("; write '") + target + " += String(" + value + ")'").str();
}

std::string ErrorTextCompound::bitsDouble(llvm::StringRef spelling) {
  return (llvm::Twine("operator '") + spelling +
          "' works on the bits of integers, and this is a double; "
          "drop the fraction first, e.g. 'Math.trunc(x).toInt()'")
      .str();
}

std::string ErrorTextCompound::nothing(llvm::StringRef spelling) {
  return (llvm::Twine("'") + spelling +
          " 0' changes nothing in tz: integers are "
          "exact, and nothing wraps around here; to "
          "count modulo 2^32, use 'Bit.Wrap32'")
      .str();
}

std::string ErrorTextCompound::signedTo(llvm::StringRef type) {
  return (llvm::Twine("'>>>=' shifts in zeros, as a shift of an unsigned "
                      "type does; '") +
          type + "' is signed: use '>>='")
      .str();
}

std::string ErrorTextCompound::mixed(llvm::StringRef spelling, llvm::StringRef type,
                                     llvm::StringRef value, llvm::StringRef hint) {
  return (llvm::Twine("operator '") + spelling + "' cannot be applied to types '" + type +
          "' and '" + value + "': a number changes its type only explicitly" + hint)
      .str();
}

std::string ErrorTextLogicalAssign::simple(llvm::StringRef op) {
  return (llvm::Twine("'") + op +
          "' needs a variable, a field or an array element at a "
          "simple index, such as 'a[i]'")
      .str();
}

std::string ErrorTextLogicalAssign::field() {
  return "'??=' needs a variable or a field of one, such as 'o.x'";
}

std::string ErrorTextLogicalAssign::fieldOpt(llvm::StringRef name, llvm::StringRef type) {
  return (llvm::Twine("'??=' needs a field that may be undefined; '") + name + "' has type '" +
          type + "'")
      .str();
}

std::string ErrorTextUpdate::notNumber(llvm::StringRef op, llvm::StringRef type) {
  return (llvm::Twine("operator '") + op + "' cannot be applied to type '" + type + "'").str();
}

std::string ErrorTextAssign::length() {
  return "an array length";
}

}
