#include "compiler/program/types/template_patterns/template_patterns.h"
#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/ADT/SmallPtrSet.h"
#include "llvm/ADT/StringSwitch.h"
#include "llvm/Support/ConvertUTF.h"
#include "llvm/Support/ErrorHandling.h"
#include <cctype>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <iterator>
#include <map>
#include <memory>
#include <tuple>

namespace tz {

static std::string utf8Text(const std::u16string &text) {
  std::string utf8;
  llvm::convertUTF16ToUTF8String(
      llvm::ArrayRef<llvm::UTF16>(reinterpret_cast<const llvm::UTF16 *>(text.data()), text.size()),
      utf8);

  return utf8;
}

bool splitByTemplate(const std::u16string &text, llvm::ArrayRef<std::u16string> texts,
                     std::vector<std::u16string> &parts) {
  const std::u16string &start = texts.front(), &end = texts.back();
  if (text.size() < start.size() + end.size() || text.compare(0, start.size(), start) != 0 ||
      text.compare(text.size() - end.size(), end.size(), end) != 0) {
    return false;
  }
  std::u16string rest = text.substr(0, text.size() - end.size());
  size_t pos = start.size();
  parts.clear();
  for (size_t i = 1; i + 1 < texts.size(); ++i) {
    const std::u16string &delimiter = texts[i];
    if (!delimiter.empty()) {
      size_t found = rest.find(delimiter, pos);
      if (found == std::u16string::npos) {
        return false;
      }
      parts.push_back(rest.substr(pos, found - pos));
      pos = found + delimiter.size();
    } else if (pos < rest.size()) {
      parts.push_back(rest.substr(pos, 1));
      pos += 1;
    } else {
      return false;
    }
  }
  parts.push_back(rest.substr(pos));

  return true;
}

bool numberString(const std::u16string &text, double &value) {
  auto space = [](char16_t c) {
    return c == ' ' || (c >= '\t' && c <= '\r') || c == 0xa0 || c == 0x1680 ||
           (c >= 0x2000 && c <= 0x200a) || c == 0x2028 || c == 0x2029 || c == 0x202f ||
           c == 0x205f || c == 0x3000 || c == 0xfeff;
  };
  size_t begin = 0, end = text.size();
  while (begin < end && space(text[begin])) {
    ++begin;
  }
  while (end > begin && space(text[end - 1])) {
    --end;
  }
  std::string ascii;
  for (size_t i = begin; i < end; ++i) {
    if (text[i] > 0x7f) {
      return false;
    }
    ascii += char(text[i]);
  }
  if (text.empty()) {
    return false;
  }
  if (ascii.empty()) {
    value = 0;
    return true;
  }
  if (ascii.size() > 2 && ascii[0] == '0' && std::strchr("xXoObB", ascii[1])) {
    int radix = ascii[1] == 'x' || ascii[1] == 'X'   ? 16
                : ascii[1] == 'o' || ascii[1] == 'O' ? 8
                                                     : 2;
    value = 0;
    for (size_t i = 2; i < ascii.size(); ++i) {
      int digit = std::isdigit(static_cast<unsigned char>(ascii[i])) ? ascii[i] - '0'
                  : std::isxdigit(static_cast<unsigned char>(ascii[i]))
                      ? std::tolower(static_cast<unsigned char>(ascii[i])) - 'a' + 10
                      : 99;
      if (digit >= radix) {
        return false;
      }
      value = value * radix + digit;
    }
    return std::isfinite(value);
  }
  size_t i = 0, digits = 0;
  if (ascii[i] == '+' || ascii[i] == '-') {
    ++i;
  }
  while (i < ascii.size() && std::isdigit(static_cast<unsigned char>(ascii[i]))) {
    ++i, ++digits;
  }
  if (i < ascii.size() && ascii[i] == '.') {
    ++i;
    while (i < ascii.size() && std::isdigit(static_cast<unsigned char>(ascii[i]))) {
      ++i, ++digits;
    }
  }
  if (digits == 0) {
    return false;
  }
  if (i < ascii.size() && (ascii[i] == 'e' || ascii[i] == 'E')) {
    ++i;
    if (i < ascii.size() && (ascii[i] == '+' || ascii[i] == '-')) {
      ++i;
    }
    size_t exponent = 0;
    while (i < ascii.size() && std::isdigit(static_cast<unsigned char>(ascii[i]))) {
      ++i, ++exponent;
    }
    if (exponent == 0) {
      return false;
    }
  }
  if (i != ascii.size()) {
    return false;
  }
  value = std::strtod(ascii.c_str(), nullptr);

  return std::isfinite(value);
}

bool holeAccepts(const Type *hole, const std::u16string &part) {
  double value = 0;
  switch (hole->kind()) {
  case Type::Kind::String:
    return !hole->isPattern() || matchesPattern(part, hole);
  case Type::Kind::Double:
    return numberString(part, value);
  case Type::Kind::Int:
    return numberString(part, value) && value == std::trunc(value) && value >= -2147483648.0 &&
           value <= 2147483647.0;
  case Type::Kind::Long:
    return numberString(part, value) && value == std::trunc(value) &&
           std::fabs(value) <= 9223372036854775807.0;
  case Type::Kind::Literal:
    return hole->isStringLiteral() ? part == hole->literalText() : utf8Text(part) == hole->name();
  case Type::Kind::Union:
    return llvm::any_of(hole->members(), [&](const Type *m) { return holeAccepts(m, part); });
  default:
    return false;
  }
}

bool matchesPattern(const std::u16string &text, const Type *pattern) {
  std::vector<std::u16string> parts;
  if (!splitByTemplate(text, pattern->patternTexts(), parts)) {
    return false;
  }
  for (size_t i = 0; i < parts.size(); ++i) {
    if (!holeAccepts(pattern->patternHoles()[i], parts[i])) {
      return false;
    }
  }

  return true;
}

}
