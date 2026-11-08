#pragma once

#include "compiler/lower/library/library_lowerer/library_lowerer.h"
#include <memory>
#include <vector>

namespace tz {

class LowererRegistry {
public:
  LowererRegistry();

  const LibraryLowerer *find(Builtin builtin) const;

private:
  std::vector<std::unique_ptr<LibraryLowerer>> lowerers;
};

}
