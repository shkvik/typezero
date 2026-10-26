#pragma once

#include "llvm/ADT/StringRef.h"
#include <cstdint>
#include <optional>
#include <string>

namespace tz {

struct DecimalLiteral {
  std::string digits;
  int64_t exponent = 0;

  bool isZero() const { return digits.empty(); }
  int64_t top() const { return exponent + static_cast<int64_t>(digits.size()) - 1; }
};

constexpr int64_t kDecimalMaxExp = 10000000;
constexpr int64_t kDecimalMinExp = -10000000;

std::optional<DecimalLiteral> readDecimalLiteral(llvm::StringRef text);

DecimalLiteral decimalOf(uint64_t value);

int decimalRoundingMode(const std::u16string &name);
constexpr int kRoundHalfUp = 4;
extern const char kRoundingModes[];

}
