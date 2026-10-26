#pragma once

#include "llvm/ADT/StringRef.h"
#include <string>

namespace tz {

std::string osExportProblem(llvm::StringRef name, llvm::StringRef path);

}
