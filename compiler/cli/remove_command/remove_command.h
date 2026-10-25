#pragma once

#include "llvm/ADT/ArrayRef.h"

namespace tz::pkg {

// tz remove <name>...
class RemoveCommand {
public:
  int run(llvm::ArrayRef<const char *> args) const;
};

} // namespace tz::pkg
