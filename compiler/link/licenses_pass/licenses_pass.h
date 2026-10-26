#pragma once

#include "compiler/link/licenses/licenses.h"
#include "llvm/IR/Module.h"

namespace tz {

class LicensesPass {
public:
  CryptoUse run(llvm::Module &module) const;
};

}
