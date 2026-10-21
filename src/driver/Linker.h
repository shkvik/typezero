#pragma once

#include "llvm/ADT/ArrayRef.h"
#include "llvm/ADT/StringRef.h"
#include "llvm/TargetParser/Triple.h"

#include <string>

namespace tz {

// Links object files and libraries into an executable with the LLD linker
// built into tz. On failure returns false, with the linker's output in `errors`.
bool linkExecutable(const llvm::Triple &triple, llvm::ArrayRef<std::string> inputs,
                    llvm::StringRef output, std::string &errors);

} // namespace tz
