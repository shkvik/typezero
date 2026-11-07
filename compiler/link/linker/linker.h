#pragma once

#include "llvm/ADT/ArrayRef.h"
#include "llvm/ADT/StringRef.h"
#include "llvm/TargetParser/Triple.h"
#include <string>

namespace tz {

class Linker {
public:
  static bool linkExecutable(const llvm::Triple &triple, llvm::ArrayRef<std::string> inputs,
                             llvm::StringRef output, bool debug, llvm::StringRef natvis,
                             std::string &errors);
};

}
