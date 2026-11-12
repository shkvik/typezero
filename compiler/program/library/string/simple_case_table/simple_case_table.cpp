#include "compiler/program/library/string/simple_case_table/simple_case_table.h"

namespace tz {

std::u16string SimpleCaseTable::upperOf(char16_t unit) {
  if (unit >= u'a' && unit <= u'z') {
    return std::u16string(1, char16_t(unit - 32));
  }
  if (unit == 0xdf) {
    return u"SS";
  }
  if (unit == 0xb5) {
    return std::u16string(1, char16_t(0x39c));
  }
  if (unit >= 0xe0 && unit <= 0xfe && unit != 0xf7) {
    return std::u16string(1, char16_t(unit - 32));
  }
  if (unit == 0xff) {
    return std::u16string(1, char16_t(0x178));
  }
  if (unit == 0x131) {
    return u"I";
  }
  if (unit == 0x17f) {
    return u"S";
  }
  if ((unit >= 0x100 && unit <= 0x137) || (unit >= 0x14a && unit <= 0x177)) {
    return std::u16string(1, char16_t(unit & ~1u));
  }
  if ((unit >= 0x139 && unit <= 0x148) || (unit >= 0x179 && unit <= 0x17e)) {
    return std::u16string(1, char16_t(unit % 2 == 0 ? unit - 1 : unit));
  }
  if (unit == 0x3c2) {
    return std::u16string(1, char16_t(0x3a3));
  }
  if ((unit >= 0x3b1 && unit <= 0x3c1) || (unit >= 0x3c3 && unit <= 0x3cb)) {
    return std::u16string(1, char16_t(unit - 32));
  }
  if (unit == 0x3ac) {
    return std::u16string(1, char16_t(0x386));
  }
  if (unit >= 0x3ad && unit <= 0x3af) {
    return std::u16string(1, char16_t(unit - 37));
  }
  if (unit == 0x3cc) {
    return std::u16string(1, char16_t(0x38c));
  }
  if (unit == 0x3cd || unit == 0x3ce) {
    return std::u16string(1, char16_t(unit - 63));
  }
  if (unit >= 0x430 && unit <= 0x44f) {
    return std::u16string(1, char16_t(unit - 32));
  }
  if (unit >= 0x450 && unit <= 0x45f) {
    return std::u16string(1, char16_t(unit - 80));
  }

  return std::u16string(1, unit);
}

std::u16string SimpleCaseTable::lowerOf(char16_t unit) {
  if (unit >= u'A' && unit <= u'Z') {
    return std::u16string(1, char16_t(unit + 32));
  }
  if (unit >= 0xc0 && unit <= 0xde && unit != 0xd7) {
    return std::u16string(1, char16_t(unit + 32));
  }
  if (unit == 0x178) {
    return std::u16string(1, char16_t(0xff));
  }
  if (unit == 0x130) {
    return std::u16string{u'i', char16_t(0x307)};
  }
  if ((unit >= 0x100 && unit <= 0x137) || (unit >= 0x14a && unit <= 0x177)) {
    return std::u16string(1, char16_t(unit | 1u));
  }
  if ((unit >= 0x139 && unit <= 0x148) || (unit >= 0x179 && unit <= 0x17e)) {
    return std::u16string(1, char16_t(unit % 2 == 1 ? unit + 1 : unit));
  }
  if ((unit >= 0x391 && unit <= 0x3a1) || (unit >= 0x3a3 && unit <= 0x3ab)) {
    return std::u16string(1, char16_t(unit + 32));
  }
  if (unit == 0x386) {
    return std::u16string(1, char16_t(0x3ac));
  }
  if (unit >= 0x388 && unit <= 0x38a) {
    return std::u16string(1, char16_t(unit + 37));
  }
  if (unit == 0x38c) {
    return std::u16string(1, char16_t(0x3cc));
  }
  if (unit == 0x38e || unit == 0x38f) {
    return std::u16string(1, char16_t(unit + 63));
  }
  if (unit >= 0x410 && unit <= 0x42f) {
    return std::u16string(1, char16_t(unit + 32));
  }
  if (unit >= 0x400 && unit <= 0x40f) {
    return std::u16string(1, char16_t(unit + 80));
  }

  return std::u16string(1, unit);
}

}
