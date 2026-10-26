#pragma once

#include "llvm/IR/Module.h"
#include <memory>

namespace tz {

struct Lowered {
  std::unique_ptr<llvm::Module> module;
  bool parallel = false;
};

}
