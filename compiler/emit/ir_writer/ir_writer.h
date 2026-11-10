#pragma once

#include "compiler/program/diagnostic/diagnostic.h"
#include "llvm/ADT/StringRef.h"
#include "llvm/IR/Module.h"
#include <vector>

namespace tz {

class IrWriter {
public:
  std::vector<Diagnostic> run(const llvm::Module &module, llvm::StringRef path) const;
};

}
