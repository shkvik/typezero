#pragma once

#include "llvm/IR/Module.h"
#include <memory>

namespace tz {

struct Lowered {
  std::unique_ptr<llvm::Module> module;
  // The program runs on the threads of the parallel runtime (tzrt_mt): some of its async code
  // may run at the same time as other (docs/async.md).
  bool parallel = false;
};

} // namespace tz
