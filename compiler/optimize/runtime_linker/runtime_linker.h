#pragma once

#include "compiler/program/errors/errors.h"
#include "llvm/IR/Module.h"
#include <string>

namespace tz {

class RuntimeLinker {
public:
  Errors run(llvm::Module &module, const std::string &path) const;

private:
  static unsigned copySize(llvm::Function &fn, llvm::SmallPtrSetImpl<llvm::Function *> &seen);
};

}
