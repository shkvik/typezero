#pragma once

#include "llvm/ADT/StringRef.h"
#include <cstdint>
#include <optional>
#include <string>

namespace tz {

// The exact value of a number literal written where a decimal is expected:
// `digits * 10^exponent`, read from the text of the program rather than from
// the double JavaScript would make of it.
struct DecimalLiteral {
  std::string digits; // no leading or trailing zeros; empty for zero
  int64_t exponent = 0;

  bool isZero() const { return digits.empty(); }
  // The exponent of the leading digit, as BigNumber.js counts it.
  int64_t top() const { return exponent + static_cast<int64_t>(digits.size()) - 1; }
};

// The range of BigNumber.js (and of runtime/library/decimal/decimal/decimal.c):
// a value whose leading digit is beyond 10^kDecimalMaxExp does not exist, one
// below 10^kDecimalMinExp is zero.
constexpr int64_t kDecimalMaxExp = 10000000;
constexpr int64_t kDecimalMinExp = -10000000;

// Reads the text of a number literal: `19.99`, `1e-7`, `1_000`, `0xff`. Null
// if the text is not one.
std::optional<DecimalLiteral> readDecimalLiteral(llvm::StringRef text);

// The same for an integer the compiler knows only by its value.
DecimalLiteral decimalOf(uint64_t value);

// The number of a rounding mode written as a string ("half-up" and the
// like), as BigNumber.js numbers them and
// runtime/library/decimal/decimal/decimal.c takes them; -1 for another string.
int decimalRoundingMode(const std::u16string &name);
constexpr int kRoundHalfUp = 4;
// The modes, for messages.
extern const char kRoundingModes[];

} // namespace tz
