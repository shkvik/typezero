#include "compiler/semantic/library/math/math_checker/math_checker_errors.h"

namespace tz {

std::string ErrorTextMath::constant(llvm::StringRef name) {
  return "'Math." + name.str() + "' is a number, not a function";
}

std::string ErrorTextMath::noMember(llvm::StringRef name) {
  return "property '" + name.str() + "' does not exist on 'Math'";
}

std::string ErrorTextMath::noValues(llvm::StringRef name) {
  return "'Math." + name.str() + "()' without values gives " + (name == "max" ? "-" : "") +
         "Infinity; pass the values, e.g. 'Math." + name.str() + "(...xs)'";
}

std::string ErrorTextMath::rounding(llvm::StringRef name, llvm::StringRef type) {
  return "'Math." + name.str() + "' rounds a double, and this is '" + type.str() +
         "', which has no fraction: '/' of integers already drops it";
}

std::string ErrorTextMath::oneType(llvm::StringRef common, llvm::StringRef type,
                                   llvm::StringRef hint) {
  return "the values of 'Math.min', 'Math.max' and 'Math.clamp' must have one "
         "type; found '" +
         common.str() + "' and '" + type.str() + "'" + hint.str();
}

std::string ErrorTextMath::spread(llvm::StringRef type) {
  return "only an array of numbers can be spread here, found '" + type.str() + "'";
}

std::string ErrorTextMath::spreadType(bool min, llvm::StringRef common, llvm::StringRef type) {
  return "the values of 'Math." + std::string(min ? "min" : "max") +
         "' must have one type; found '" + common.str() + "' and '" + type.str() + "'";
}

std::string ErrorTextBit::noMember(llvm::StringRef name) {
  return "property '" + name.str() + "' does not exist on 'Bit'";
}

std::string ErrorTextBit::onlyFrom(llvm::StringRef name) {
  return "'Bit." + name.str() + "' has only 'from': 'Bit." + name.str() + ".from(x)'";
}

std::string ErrorTextBit::fromInt(llvm::StringRef name, llvm::StringRef type) {
  return "'Bit." + name.str() + ".from' takes an integer, found '" + type.str() + "'";
}

std::string ErrorTextBit::label(llvm::StringRef name) {
  return "Bit." + name.str();
}

std::string ErrorTextBit::uint64(llvm::StringRef what, llvm::StringRef type, llvm::StringRef hint) {
  return "'" + what.str() + "' takes a 'uint64', found '" + type.str() + "'" + hint.str();
}

std::string ErrorTextBit::count(llvm::StringRef what, llvm::StringRef type) {
  return "the count of '" + what.str() + "' is an integer, found '" + type.str() + "'";
}

std::string ErrorTextMathArg::notDouble(const Type *type) {
  std::string message = "argument of type '" + type->name().str() +
                        "' is not assignable to parameter of type 'double'";
  if (type->isOptional() && type->inner()->isNumeric()) {
    message += "; the value may be undefined: check it first or give a default with '??'";
  } else if (type->isDecimal()) {
    message += "; convert it with 'Number(x)', or use what decimals have: 'x.abs()', 'x.sqrt()', "
               "'x.round()', 'decimal.max(a, b)'...";
  }

  return message;
}

std::string ErrorTextMathArg::bits(llvm::StringRef what, unsigned bits, llvm::StringRef type,
                                   llvm::StringRef hint) {
  std::string names = bits == 32 ? "int32, uint32 or Bit.Wrap32" : "int, uint64 or Bit.Wrap64";

  return "'" + what.str() + "' takes a " + std::to_string(bits) + "-bit integer (" + names +
         "), found '" + type.str() + "'" + hint.str();
}

std::string ErrorTextMathArg::trunc(llvm::StringRef method) {
  return "; drop the fraction first, e.g. 'Math.trunc(x)." + method.str() + "()'";
}

}
