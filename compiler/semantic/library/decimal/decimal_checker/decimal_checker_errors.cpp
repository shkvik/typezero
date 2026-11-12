#include "compiler/semantic/library/decimal/decimal_checker/decimal_checker_errors.h"
#include "compiler/program/library/decimal/decimal_literal/decimal_literal.h"
#include "compiler/semantic/types/type_relations/type_relations.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/ADT/Twine.h"

namespace tz {

namespace {

struct Replacement {
  llvm::StringLiteral name, use;
};

const Replacement kReplacements[] = {
    {"plus", "the operator: 'a + b'"},
    {"minus", "the operator: 'a - b'"},
    {"times", "the operator: 'a * b'"},
    {"multipliedBy", "the operator: 'a * b'"},
    {"dividedBy", "the operator 'a / b', or 'a.div(b, places, mode)'"},
    {"dividedToIntegerBy", "'a.div(b, 0, \"down\")'"},
    {"idiv", "'a.div(b, 0, \"down\")'"},
    {"modulo", "the operator: 'a % b'"},
    {"mod", "the operator: 'a % b'"},
    {"exponentiatedBy", "the operator: 'a ** n'"},
    {"pow", "the operator: 'a ** n'"},
    {"negated", "the operator: '-a'"},
    {"comparedTo", "the operators: 'a < b', 'a == b', 'a > b'"},
    {"isEqualTo", "the operator: 'a == b'"},
    {"eq", "the operator: 'a == b'"},
    {"isGreaterThan", "the operator: 'a > b'"},
    {"gt", "the operator: 'a > b'"},
    {"isGreaterThanOrEqualTo", "the operator: 'a >= b'"},
    {"gte", "the operator: 'a >= b'"},
    {"isLessThan", "the operator: 'a < b'"},
    {"lt", "the operator: 'a < b'"},
    {"isLessThanOrEqualTo", "the operator: 'a <= b'"},
    {"lte", "the operator: 'a <= b'"},
    {"isZero", "a comparison: 'a == 0'"},
    {"isNegative", "a comparison: 'a < 0'"},
    {"isPositive", "a comparison: 'a > 0'"},
    {"absoluteValue", "'a.abs()'"},
    {"squareRoot", "'a.sqrt()'"},
    {"integerValue", "'a.round(0, mode)'"},
    {"dp", "'a.decimalPlaces()', or 'a.round(places, mode)' to round"},
    {"precision", "'a.toPrecision(digits)'"},
    {"sd", "'a.toPrecision(digits)'"},
    {"toNumber", "'Number(a)'"},
    {"valueOf", "'String(a)'"},
    {"toJSON", "'String(a)'"},
};

}

llvm::StringRef ErrorTextDecimalMethod::replacementFor(llvm::StringRef name) {
  for (const Replacement &replacement : llvm::reverse(kReplacements)) {
    if (replacement.name == name) {
      return replacement.use;
    }
  }

  return llvm::StringRef();
}

std::string ErrorTextBigInt::none(llvm::StringRef what) {
  return what.str() + " is not supported, as there is no bigint: integers up to 9.2e18 fit in "
                      "'int', longer ones in 'decimal' (e.g. 'decimal(2) ** 100')";
}

std::string ErrorTextBigInt::quoted(llvm::StringRef name) {
  return "'" + name.str() + "'";
}

std::string ErrorTextDecimalLit::unreadable() {
  return "this number cannot be read as a decimal";
}

std::string ErrorTextDecimalLit::outOfRange(int64_t digits) {
  return "this decimal is out of range: a decimal has at most " + std::to_string(digits) +
         " digits before the point";
}

std::string ErrorTextDecimalMix::integer(llvm::StringRef op, llvm::StringRef left,
                                         llvm::StringRef right, llvm::StringRef lhs,
                                         llvm::StringRef rhs, bool leftDecimal) {
  std::string message = (llvm::Twine("operator '") + op + "' cannot be applied to types '" + left +
                         "' and '" + right + "': a number changes its type only explicitly")
                            .str();
  if (!lhs.empty() && !rhs.empty()) {
    message += leftDecimal ? (", e.g. '" + lhs + " " + op + " " + TypeRelations::operandText(rhs) +
                              ".toDecimal()'")
                                 .str()
                           : (", e.g. '" + TypeRelations::operandText(lhs) + ".toDecimal() " + op +
                              " " + rhs + "'")
                                 .str();
  } else {
    message += " with '.toDecimal()'";
  }

  return message;
}

std::string ErrorTextDecimalMix::inexact(llvm::StringRef op, llvm::StringRef left,
                                         llvm::StringRef right, llvm::StringRef lhs,
                                         llvm::StringRef rhs, bool leftDecimal, bool example) {
  std::string message = (llvm::Twine("operator '") + op + "' cannot be applied to types '" + left +
                         "' and '" + right +
                         "': a double is inexact already, and a decimal would take that on "
                         "silently; convert it explicitly")
                            .str();
  if (example && !lhs.empty() && !rhs.empty()) {
    message += leftDecimal ? (": '" + lhs + " " + op + " decimal(" + rhs + ")'").str()
                           : (": 'decimal(" + lhs + ") " + op + " " + rhs + "'").str();
  } else {
    message += " with 'decimal(x)'";
  }

  return message;
}

std::string ErrorTextDecimalPower::exponent(const Type *found) {
  std::string message = "the exponent of a decimal power must be an integer ('int'), found '" +
                        found->name().str() + "'";
  if (found->isFloat() || found->isDecimal()) {
    message += "; for a square root, use 'x.sqrt()'";
  }

  return message;
}

std::string ErrorTextDecimalPower::divZero() {
  return "decimal division by zero; a decimal has no Infinity or NaN";
}

std::string ErrorTextDecimalOf::notNumber(llvm::StringRef text) {
  return "'" + text.str() +
         "' is no decimal number; write it as one, e.g. "
         "'12.50', '-1e3' or '0x1f'";
}

std::string ErrorTextDecimalOf::takes(llvm::StringRef type, bool optional) {
  std::string message =
      "decimal() takes a string, a number or a decimal, found '" + type.str() + "'";
  if (optional) {
    message += "; the value may be undefined: check it first or give a default with '??'";
  }

  return message;
}

std::string ErrorTextDecimalStatic::noMember(llvm::StringRef name) {
  std::string message = "property '" + name.str() + "' does not exist on 'decimal'";
  if (name == "maximum" || name == "minimum") {
    message += "; use 'decimal." + name.substr(0, 3).str() + "'";
  } else if (name == "config" || name == "set") {
    message += ": there are no settings for all decimals; division takes its places and "
               "rounding as arguments, 'a.div(b, places, mode)'";
  }

  return message;
}

std::string ErrorTextDecimalStatic::spread(llvm::StringRef type) {
  return "only an array of decimals can be spread here, found '" + type.str() + "'";
}

std::string ErrorTextDecimalStatic::noValue(llvm::StringRef name) {
  return "decimal." + name.str() + "() needs at least one value";
}

std::string ErrorTextRoundingMode::unknown(llvm::StringRef mode) {
  return "unknown rounding mode '" + mode.str() + "'; the modes are " + kRoundingModes;
}

std::string ErrorTextRoundingMode::notKnown() {
  return std::string("a rounding mode known only when the program "
                     "runs may be no mode; check it first, e.g. "
                     "'if (mode == \"up\" || mode == \"down\")'; the "
                     "modes are ") +
         kRoundingModes;
}

std::string ErrorTextDecimalCount::notInt(llvm::StringRef what, llvm::StringRef type,
                                          bool optional) {
  std::string message = what.str() + " must be an integer ('int'), found '" + type.str() + "'";
  if (optional) {
    message += "; the value may be undefined: check it first or give a default with '??'";
  }

  return message;
}

std::string ErrorTextDecimalCount::places() {
  return "the number of decimal places";
}

std::string ErrorTextDecimalCount::digits() {
  return "the number of significant digits";
}

std::string ErrorTextDecimalCount::base() {
  return "the base";
}

std::string ErrorTextDecimalCount::power() {
  return "the power of ten";
}

std::string ErrorTextDecimalMethod::noMember(llvm::StringRef name, llvm::StringRef use) {
  std::string message = "property '" + name.str() + "' does not exist on type 'decimal'";
  if (!use.empty()) {
    message += "; a decimal uses " + use.str();
  }
  if (name == "isNaN" || name == "isFinite") {
    message += "; a decimal is never NaN or infinite";
  }

  return message;
}

std::string ErrorTextDecimalMethod::places() {
  return "decimalPlaces() only counts the places; to round, use "
         "'x.round(places, mode)'";
}

}
