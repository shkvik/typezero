#pragma once

#include <string>

namespace tz {

struct Module;

class LoadChain {
public:
  static std::string describe(const Module *reader, const Module *source);
};

}
