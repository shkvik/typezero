#pragma once

#include "llvm/ADT/ArrayRef.h"

namespace tz::pkg {

struct Lock;

class UpdateCommand {
public:
  int run(llvm::ArrayRef<const char *> args) const;

private:
  static void printChanges(const Lock &before, const Lock &after);
};

}
