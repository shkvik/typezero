#pragma once

#include "llvm/ADT/StringRef.h"
#include <string>

namespace tz::pkg {

std::string canonical(llvm::StringRef dir);

}
