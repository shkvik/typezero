#pragma once

#include "compiler/program/errors/errors.h"
#include "llvm/ADT/StringRef.h"
#include "llvm/IR/Module.h"

namespace tz {

class IrWriter {
public:
  Errors run(const llvm::Module &module, llvm::StringRef path) const;
};

}
