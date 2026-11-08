#include "compiler/semantic/bodies/expr/binary_checker/binary_checker_errors.h"
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

std::string ErrorTextMixedNumbers::mixed(llvm::StringRef op, llvm::StringRef lhs,
                                         llvm::StringRef rhs, llvm::StringRef example) {
  return (llvm::Twine("operator '") + op + "' cannot be applied to types '" + lhs + "' and '" +
          rhs + "': a number changes its type only explicitly" + example)
      .str();
}

std::string ErrorTextMixedNumbers::example(llvm::StringRef left, llvm::StringRef op,
                                           llvm::StringRef right) {
  return (llvm::Twine(", e.g. '") + left + " " + op + " " + right + "'").str();
}

std::string ErrorTextMixedNumbers::method(llvm::StringRef method) {
  return (llvm::Twine(" with '.") + method + "()'").str();
}

std::string ErrorTextTypeOfName::noNumber(llvm::StringRef here) {
  return (llvm::Twine("typeof gives no \"number\": each number type has its own name, "
                      "\"int\", \"double\", \"decimal\", \"int32\"...; the type 'number' is "
                      "'double', and its typeof is \"double\"") +
          here)
      .str();
}

std::string ErrorTextTypeOfName::forType(llvm::StringRef type, llvm::StringRef names) {
  return (llvm::Twine("; for a value of type '") + type + "' it is " + names).str();
}

std::string ErrorTextTypeOfName::noSymbol(llvm::StringRef names) {
  return (llvm::Twine("typeof gives no \"symbol\": tz has no symbols as values; the names are ") +
          names)
      .str();
}

std::string ErrorTextTypeOfName::notName(llvm::StringRef name, llvm::StringRef names) {
  return (llvm::Twine("'") + name + "' is not a name that typeof gives; the names are " + names)
      .str();
}

std::string ErrorTextTypeOfName::never(llvm::StringRef type, llvm::StringRef names,
                                       llvm::StringRef name) {
  return (llvm::Twine("typeof of a value of type '") + type + "' is " + names + ", never \"" +
          name + "\"")
      .str();
}

std::string ErrorTextNoOpBits::nothing(llvm::StringRef lhs, llvm::StringRef op,
                                       llvm::StringRef hint) {
  return (llvm::Twine("'") + lhs + " " + op +
          " 0' changes nothing in tz: integers are exact, and nothing wraps around here" + hint)
      .str();
}

std::string ErrorTextNoOpBits::toInt(llvm::StringRef lhs) {
  return (llvm::Twine("; to make it an int, write '") + lhs + ".toInt()'").str();
}

std::string ErrorTextNoOpBits::trunc(llvm::StringRef lhs) {
  return (llvm::Twine("; to drop the fraction, write 'Math.trunc(") + lhs + ").toInt()'").str();
}

std::string ErrorTextNoOpBits::wrap(llvm::StringRef text, bool unsignedShift) {
  std::string uint32 =
      unsignedShift ? (llvm::Twine(", a uint32 '") + text + ".toUint32()'").str() : std::string();

  return (llvm::Twine("; to make it an int32, write '") + text + ".toInt32()'" + uint32 +
          ", and to count modulo 2^32, use 'Bit.Wrap32'")
      .str();
}

std::string ErrorTextBitsDouble::fraction(llvm::StringRef op, llvm::StringRef side) {
  return (llvm::Twine("operator '") + op +
          "' works on the bits of integers, and this is a double; drop the "
          "fraction first, e.g. 'Math.trunc(" +
          side + ").toInt()'")
      .str();
}

std::string ErrorTextShift::isSigned(llvm::StringRef type, llvm::StringRef text,
                                     llvm::StringRef method, llvm::StringRef count) {
  return (llvm::Twine("'>>>' shifts in zeros, as a shift of an unsigned type does; '") + type +
          "' is signed: use '>>', or convert it, e.g. '" + text + "." + method + "() >>> " + count +
          "'")
      .str();
}

std::string ErrorTextPower::negative(llvm::StringRef type) {
  return (llvm::Twine("'") + type +
          "' raised to a negative power is a fraction, which an "
          "integer cannot hold; use a double, e.g. '2.0 ** -1'")
      .str();
}

std::string ErrorTextResultEquality::compared(llvm::StringRef op) {
  return (llvm::Twine("Results cannot be compared with '") + op +
          "': in JavaScript 'ok(1) == ok(1)' is false, as they are two "
          "objects; compare what they hold, with 'isOk()', 'match' or "
          "'value'")
      .str();
}

std::string ErrorTextTypeOfName::bigint() {
  return "'typeof x == \"bigint\"'";
}

}
