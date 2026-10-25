#pragma once

#include "llvm/ADT/ArrayRef.h"

namespace tz::pkg {

// tz update [name...]: the newest versions tz.json allows.
class UpdateCommand {
public:
  int run(llvm::ArrayRef<const char *> args) const;
};

} // namespace tz::pkg
