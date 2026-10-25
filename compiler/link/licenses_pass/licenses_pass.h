#pragma once

#include "compiler/link/licenses/licenses.h"
#include "llvm/IR/Module.h"

namespace tz {

// What code of others goes into the executable: the libraries of node:crypto it needs, and the
// manifest that `tz licenses` reads from it.
class LicensesPass {
public:
  CryptoUse run(llvm::Module &module) const;
};

} // namespace tz
