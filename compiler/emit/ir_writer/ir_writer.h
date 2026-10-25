#pragma once

#include "compiler/program/errors/errors.h"
#include "llvm/ADT/StringRef.h"
#include "llvm/IR/Module.h"

namespace tz {

// `tz build --emit-llvm`: the module as LLVM IR text (.ll) instead of an executable.
class IrWriter {
public:
  Errors run(const llvm::Module &module, llvm::StringRef path) const;
};

} // namespace tz
