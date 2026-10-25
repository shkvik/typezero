#pragma once

#include "llvm/ADT/ArrayRef.h"

namespace tz::pkg {

// tz init [directory]: tz.json, main.tz and .gitignore.
class InitCommand {
public:
  int run(llvm::ArrayRef<const char *> args) const;
};

} // namespace tz::pkg
