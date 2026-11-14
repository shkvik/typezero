#include "compiler/semantic/code/arithmetic/code_arithmetic_registry_errors.h"
#include "llvm/ADT/Twine.h"

namespace tz {

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

std::string ErrorTextDivisor::zero() {
  return "division by zero";
}

std::string ErrorTextShiftCount::outside(int64_t count, int last, llvm::StringRef type) {
  return (llvm::Twine("the shift count ") + llvm::Twine(count) + " is outside 0 to " +
          llvm::Twine(last) + ", the bits of '" + type + "'")
      .str();
}

std::string ErrorTextOverflow::inInt(llvm::StringRef value, llvm::StringRef type, int64_t wrapped) {
  return (llvm::Twine("this constant is ") + value + ", which does not fit in '" + type +
          "' and would wrap around to " + llvm::Twine(wrapped) +
          "; compute it as an 'int', e.g. 'const x: int = ...'")
      .str();
}

std::string ErrorTextOverflow::inDouble(llvm::StringRef value, llvm::StringRef type,
                                        int64_t wrapped) {
  return (llvm::Twine("this constant is ") + value + ", which does not fit in '" + type +
          "' and would wrap around to " + llvm::Twine(wrapped) +
          "; compute it as a 'double', e.g. 'const x: double = ...'")
      .str();
}

}
