#pragma once

#include "llvm/ADT/StringRef.h"
#include <string>

namespace tz {

std::string fsExportProblem(llvm::StringRef name, llvm::StringRef path);
std::string fsPromisesExportProblem(llvm::StringRef name, llvm::StringRef path);
std::string readlineExportProblem(llvm::StringRef name, llvm::StringRef path, bool hasPromises);
std::string tzIoExportProblem(llvm::StringRef name, llvm::StringRef path);

}
