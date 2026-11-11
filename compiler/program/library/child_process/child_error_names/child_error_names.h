#pragma once

#include "llvm/ADT/StringRef.h"
#include <vector>

namespace tz {

class ChildErrorNames {
public:
  static std::vector<llvm::StringRef> getTypeNames();
};

}
