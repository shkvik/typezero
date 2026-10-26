#pragma once

#include "llvm/ADT/ArrayRef.h"
#include "llvm/ADT/StringRef.h"
#include "llvm/TargetParser/Triple.h"
#include <string>

namespace tz {

bool linkExecutable(const llvm::Triple &triple, llvm::ArrayRef<std::string> inputs,
                    llvm::StringRef output, std::string &errors);

}
