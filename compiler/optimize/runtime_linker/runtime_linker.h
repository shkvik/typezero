#pragma once

#include "compiler/program/diagnostic/diagnostic.h"
#include "llvm/IR/Module.h"
#include <string>
#include <vector>

namespace tz {

class RuntimeLinker {
public:
  std::vector<Diagnostic> run(llvm::Module &module, const std::string &path) const;

private:
  static unsigned copySize(llvm::Function &fn, llvm::SmallPtrSetImpl<llvm::Function *> &seen);
};

}
