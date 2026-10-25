#pragma once

#include "llvm/ADT/ArrayRef.h"

namespace tz::pkg {

// tz add <package>...: adds packages to tz.json and tz.lock.
class AddCommand {
public:
  int run(llvm::ArrayRef<const char *> args) const;
};

} // namespace tz::pkg
