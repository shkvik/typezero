#pragma once

#include "llvm/ADT/StringRef.h"
#include <string>

namespace tz::pkg {

std::string capitalize(llvm::StringRef s);

std::string derivedName(llvm::StringRef file, llvm::StringRef packageDir,
                        llvm::StringRef packageName);

}
