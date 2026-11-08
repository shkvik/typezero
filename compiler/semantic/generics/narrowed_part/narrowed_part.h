#pragma once

#include <string>

namespace tz {

class Type;

struct NarrowedPart {
  int depth;
  bool element;
  std::string name;
  const Type *narrow;
  const Type *owner;
};

}
