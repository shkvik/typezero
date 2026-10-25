#pragma once

#include "compiler/program/errors/errors.h"
#include "llvm/IR/Module.h"
#include <string>

namespace tz {

// Copies the hot runtime functions (TZ_HOT in runtime/core/tzrt/tzrt.h) the program calls, and
// the hot ones they call, from the runtime bitcode into the program's module as
// available_externally: the optimizer may inline them into the program's loops, and then drops
// them, so every call that is left goes to tzrt.lib, which is linked as usual.
class RuntimeLinker {
public:
  Errors run(llvm::Module &module, const std::string &path) const;
};

} // namespace tz
