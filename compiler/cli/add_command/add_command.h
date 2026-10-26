#pragma once

#include "llvm/ADT/ArrayRef.h"

namespace tz::pkg {

class AddCommand {
public:
  int run(llvm::ArrayRef<const char *> args) const;
};

}
