#pragma once

#include "llvm/ADT/StringRef.h"
#include <cstdint>
#include <optional>
#include <string>

namespace tz {

class Expr;

struct DecimalLiteral {
  static std::optional<DecimalLiteral> read(llvm::StringRef text);
  static DecimalLiteral of(uint64_t value);
  static std::optional<DecimalLiteral> fromExpr(const Expr *expr);
  static int roundingMode(const std::u16string &name);
  static DecimalLiteral normalized(std::string digits, int64_t exponent);

  std::string digits;
  int64_t exponent = 0;

  bool isZero() const { return digits.empty(); }
  int64_t top() const { return exponent + static_cast<int64_t>(digits.size()) - 1; }
};

constexpr int64_t kDecimalMaxExp = 10000000;
constexpr int64_t kDecimalMinExp = -10000000;

constexpr int kRoundHalfUp = 4;
extern const char kRoundingModes[];

}
