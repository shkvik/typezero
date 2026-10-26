#pragma once

#include "llvm/ADT/ArrayRef.h"
#include "llvm/ADT/StringRef.h"
#include <cstdint>
#include <string>
#include <vector>

namespace tz {

class Type;

struct Overload {
  std::string name;
  std::vector<const Type *> signatures;
};

}
