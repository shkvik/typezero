#pragma once

#include <string>

namespace tz {

class SimpleCaseTable {
public:
  static std::u16string upperOf(char16_t unit);
  static std::u16string lowerOf(char16_t unit);
};

}
