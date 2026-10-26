#pragma once

#include "llvm/ADT/StringRef.h"
#include <string>

namespace tz::pkg {

bool readText(llvm::StringRef file, std::string &text);

}
