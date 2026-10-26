#pragma once

#include "llvm/ADT/StringRef.h"
#include <string>

namespace tz {

std::string pathExportProblem(llvm::StringRef name, llvm::StringRef path);

}
