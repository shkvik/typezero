#pragma once

#include "compiler/load/parser/parser.h"

namespace tz {

class ToUtf8 {
public:
  static std::string toUtf8(const std::u16string &text);
};

}
