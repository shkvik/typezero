#pragma once

#include "llvm/ADT/StringRef.h"
#include <string>

namespace tz::pkg {

std::string canonical(llvm::StringRef dir);
std::string findAbove(llvm::StringRef dir, llvm::StringRef name);

}
