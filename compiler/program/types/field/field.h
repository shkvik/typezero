#pragma once

#include "llvm/ADT/ArrayRef.h"
#include "llvm/ADT/StringRef.h"
#include <cstdint>
#include <string>
#include <vector>

namespace tz {

class Type;

struct Field {
  std::string name;
  const Type *type = nullptr;
  bool readonly = false;
};

}
