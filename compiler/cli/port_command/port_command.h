#pragma once

#include "llvm/ADT/ArrayRef.h"

namespace tz::pkg {

// tz port <package> [-o directory] [--in-place]: copies a TypeScript library and adapts it
// to tz: what can be changed by rules is changed, the rest is listed for the person who
// ports it (docs/packages.md).
class PortCommand {
public:
  int run(llvm::ArrayRef<const char *> args) const;
};

} // namespace tz::pkg
