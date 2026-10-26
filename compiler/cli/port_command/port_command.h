#pragma once

#include "llvm/ADT/ArrayRef.h"

namespace tz::pkg {

class PortCommand {
public:
  int run(llvm::ArrayRef<const char *> args) const;
};

}
