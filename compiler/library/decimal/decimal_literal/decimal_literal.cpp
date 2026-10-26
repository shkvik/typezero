#include "compiler/library/decimal/decimal_literal/decimal_literal.h"
#include "llvm/ADT/APInt.h"
#include "llvm/ADT/SmallString.h"
#include "llvm/ADT/StringExtras.h"

namespace tz {

namespace {

DecimalLiteral normalized(std::string digits, int64_t exponent) {
  size_t first = digits.find_first_not_of('0');
  if (first == std::string::npos) {
    return {};
  }
  size_t last = digits.find_last_not_of('0');
  exponent += static_cast<int64_t>(digits.size() - 1 - last);

  return {digits.substr(first, last + 1 - first), exponent};
}

}

std::optional<DecimalLiteral> readDecimalLiteral(llvm::StringRef text) {
  std::string clean;
  for (char c : text) {
    if (c != '_') {
      clean.push_back(c);
    }
  }
  llvm::StringRef s = clean;

  char kind = s.size() > 2 && s[0] == '0' ? static_cast<char>(s[1] | 0x20) : 0;
  if (kind == 'x' || kind == 'o' || kind == 'b') {
    unsigned radix = kind == 'x' ? 16 : kind == 'o' ? 8 : 2;
    llvm::StringRef body = s.drop_front(2);
    for (char c : body) {
      if (llvm::hexDigitValue(c) >= radix) {
        return std::nullopt;
      }
    }
    unsigned bits = static_cast<unsigned>(body.size()) * 4 + 1;
    llvm::SmallString<64> digits;
    llvm::APInt(bits, body, static_cast<uint8_t>(radix)).toStringUnsigned(digits, 10);
    return normalized(std::string(digits), 0);
  }

  std::string digits;
  int64_t exponent = 0;
  size_t i = 0;
  bool point = false, any = false;
  for (; i < s.size() && s[i] != 'e' && s[i] != 'E'; ++i) {
    if (s[i] == '.' && !point) {
      point = true;
    } else if (llvm::isDigit(s[i])) {
      digits.push_back(s[i]);
      any = true;
      if (point) {
        --exponent;
      }
    } else {
      return std::nullopt;
    }
  }
  if (!any) {
    return std::nullopt;
  }
  if (i < s.size()) {
    ++i;
    bool negative = i < s.size() && s[i] == '-';
    if (i < s.size() && (s[i] == '+' || s[i] == '-')) {
      ++i;
    }
    if (i == s.size()) {
      return std::nullopt;
    }
    int64_t power = 0;
    for (; i < s.size(); ++i) {
      if (!llvm::isDigit(s[i])) {
        return std::nullopt;
      }
      if (power < (int64_t(1) << 40)) {
        power = power * 10 + (s[i] - '0');
      }
    }
    exponent += negative ? -power : power;
  }

  return normalized(std::move(digits), exponent);
}

DecimalLiteral decimalOf(uint64_t value) { return normalized(std::to_string(value), 0); }

const char kRoundingModes[] = "'up', 'down', 'ceil', 'floor', 'half-up', 'half-down', 'half-even', "
                              "'half-ceil' and 'half-floor'";

int decimalRoundingMode(const std::u16string &name) {
  static const char16_t *const modes[] = {u"up",        u"down",      u"ceil",
                                          u"floor",     u"half-up",   u"half-down",
                                          u"half-even", u"half-ceil", u"half-floor"};
  for (int i = 0; i < 9; ++i) {
    if (name == modes[i]) {
      return i;
    }
  }

  return -1;
}

}
